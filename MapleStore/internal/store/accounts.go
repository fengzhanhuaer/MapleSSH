package store

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"

	"golang.org/x/crypto/argon2"
)

type User struct {
	Username string `json:"username"`
	Admin    bool   `json:"admin"`
	ReadOnly bool   `json:"readOnly"`
	Disabled bool   `json:"disabled"`
	Revision uint64 `json:"revision"`
}

type AccountPatch struct {
	Password string `json:"password"`
	ReadOnly *bool  `json:"readOnly"`
	Disabled *bool  `json:"disabled"`
}

type accountRecord struct {
	User         User   `json:"user"`
	PasswordHash string `json:"passwordHash"`
}

type Accounts struct {
	mu        sync.RWMutex
	directory string
	admin     string
	users     map[string]accountRecord
	dummyHash string
}

var usernamePattern = regexp.MustCompile(`^[a-z][a-z0-9_-]{2,31}$`)

func ValidateUsername(username string) error {
	if !usernamePattern.MatchString(username) {
		return errors.New("账号须为 3–32 位小写字母、数字、下划线或连字符，且以字母开头")
	}
	switch username {
	case "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9":
		return errors.New("账号名是系统保留名称")
	}
	return nil
}

func OpenAccounts(directory, admin, password string) (*Accounts, error) {
	if err := ValidateUsername(admin); err != nil {
		return nil, fmt.Errorf("管理员账号: %w", err)
	}
	if err := validatePassword(password); err != nil {
		return nil, fmt.Errorf("MAPLESTORE_ADMIN_PASSWORD: %w", err)
	}
	if err := os.MkdirAll(directory, 0700); err != nil {
		return nil, err
	}
	accounts := &Accounts{directory: directory, admin: admin, users: make(map[string]accountRecord)}
	content, err := os.ReadFile(filepath.Join(directory, "accounts.json"))
	if err == nil {
		if len(content) > 4<<20 {
			return nil, errors.New("账号配置文件过大")
		}
		if err := json.Unmarshal(content, &accounts.users); err != nil {
			return nil, fmt.Errorf("读取账号配置: %w", err)
		}
		if accounts.users == nil || len(accounts.users) > 128 {
			return nil, errors.New("账号配置无效")
		}
		for username, record := range accounts.users {
			if ValidateUsername(username) != nil || record.User.Username != username || record.User.Admin != (username == admin) {
				return nil, errors.New("管理员账号名不能直接修改；请保持原 MAPLESTORE_ADMIN_USERNAME")
			}
		}
	} else if !os.IsNotExist(err) {
		return nil, err
	}
	adminRecord, exists := accounts.users[admin]
	if !exists || !verifyPassword(adminRecord.PasswordHash, password) {
		hashed, err := hashPassword(password)
		if err != nil {
			return nil, err
		}
		accounts.users[admin] = accountRecord{User: User{Username: admin, Admin: true, Revision: adminRecord.User.Revision + 1}, PasswordHash: hashed}
		if err := accounts.save(accounts.users); err != nil {
			return nil, err
		}
	}
	accounts.dummyHash, err = hashPassword("unused-unknown-account-password")
	if err != nil {
		return nil, err
	}
	return accounts, nil
}

func (accounts *Accounts) Lookup(username string) (User, bool) {
	accounts.mu.RLock()
	defer accounts.mu.RUnlock()
	record, exists := accounts.users[username]
	return record.User, exists
}

func (accounts *Accounts) List() []User {
	accounts.mu.RLock()
	defer accounts.mu.RUnlock()
	users := make([]User, 0, len(accounts.users))
	for _, record := range accounts.users {
		users = append(users, record.User)
	}
	sort.Slice(users, func(first, second int) bool { return users[first].Username < users[second].Username })
	return users
}

func (accounts *Accounts) Authenticate(username, password string) (User, bool) {
	accounts.mu.RLock()
	record, exists := accounts.users[username]
	dummy := accounts.dummyHash
	accounts.mu.RUnlock()
	hash := record.PasswordHash
	if !exists {
		hash = dummy
	}
	valid := verifyPassword(hash, password)
	current, currentExists := accounts.Lookup(username)
	return current, valid && exists && currentExists && !current.Disabled && current.Revision == record.User.Revision
}

func (accounts *Accounts) Create(username, password string, readOnly bool) error {
	if err := ValidateUsername(username); err != nil {
		return err
	}
	hashed, err := hashPassword(password)
	if err != nil {
		return err
	}
	accounts.mu.Lock()
	defer accounts.mu.Unlock()
	if _, exists := accounts.users[username]; exists {
		return errors.New("账号已存在")
	}
	if len(accounts.users) >= 128 {
		return errors.New("最多支持 128 个账号")
	}
	users := accounts.copyUsers()
	users[username] = accountRecord{User: User{Username: username, ReadOnly: readOnly, Revision: 1}, PasswordHash: hashed}
	if err := accounts.save(users); err != nil {
		return err
	}
	accounts.users = users
	return nil
}

func (accounts *Accounts) Update(username string, patch AccountPatch) error {
	if username == accounts.admin {
		return errors.New("管理员凭据由 Compose 环境变量管理，修改后重启生效")
	}
	var hashed string
	if patch.Password != "" {
		var err error
		hashed, err = hashPassword(patch.Password)
		if err != nil {
			return err
		}
	}
	accounts.mu.Lock()
	defer accounts.mu.Unlock()
	record, exists := accounts.users[username]
	if !exists {
		return errors.New("账号不存在")
	}
	if hashed != "" {
		record.PasswordHash = hashed
	}
	if patch.ReadOnly != nil {
		record.User.ReadOnly = *patch.ReadOnly
	}
	if patch.Disabled != nil {
		record.User.Disabled = *patch.Disabled
	}
	record.User.Revision++
	users := accounts.copyUsers()
	users[username] = record
	if err := accounts.save(users); err != nil {
		return err
	}
	accounts.users = users
	return nil
}

func (accounts *Accounts) copyUsers() map[string]accountRecord {
	users := make(map[string]accountRecord, len(accounts.users))
	for username, record := range accounts.users {
		users[username] = record
	}
	return users
}

func (accounts *Accounts) save(users map[string]accountRecord) error {
	content, err := json.MarshalIndent(users, "", "  ")
	if err != nil {
		return err
	}
	file, err := os.CreateTemp(accounts.directory, ".accounts-*")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	if _, err = file.Write(content); err == nil {
		err = file.Sync()
	}
	closeErr := file.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	return os.Rename(file.Name(), filepath.Join(accounts.directory, "accounts.json"))
}

func validatePassword(password string) error {
	if len(password) < 12 || len(password) > 1024 {
		return errors.New("密码长度须为 12–1024 字节")
	}
	return nil
}

func hashPassword(password string) (string, error) {
	if err := validatePassword(password); err != nil {
		return "", err
	}
	salt := make([]byte, 16)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}
	key := argon2.IDKey([]byte(password), salt, 2, 19*1024, 1, 32)
	return "$argon2id$v=19$m=19456,t=2,p=1$" + base64.RawStdEncoding.EncodeToString(salt) + "$" + base64.RawStdEncoding.EncodeToString(key), nil
}

func verifyPassword(encoded, password string) bool {
	parts := strings.Split(encoded, "$")
	if len(parts) != 6 || parts[1] != "argon2id" || parts[2] != "v=19" || parts[3] != "m=19456,t=2,p=1" || len(password) > 1024 {
		return false
	}
	salt, err := base64.RawStdEncoding.DecodeString(parts[4])
	if err != nil || len(salt) != 16 {
		return false
	}
	expected, err := base64.RawStdEncoding.DecodeString(parts[5])
	if err != nil || len(expected) != 32 {
		return false
	}
	actual := argon2.IDKey([]byte(password), salt, 2, 19*1024, 1, 32)
	return subtle.ConstantTimeCompare(actual, expected) == 1
}
