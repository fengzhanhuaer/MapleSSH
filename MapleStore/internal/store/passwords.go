package store

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"encoding/base64"
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

func openPasswordCipher(directory string, create bool) (cipher.AEAD, error) {
	path := filepath.Join(directory, "password.key")
	key, err := os.ReadFile(path)
	if os.IsNotExist(err) && create {
		key = make([]byte, 32)
		if _, err := rand.Read(key); err != nil {
			return nil, err
		}
		file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			return nil, err
		}
		if _, err = file.Write(key); err == nil {
			err = file.Sync()
		}
		closeErr := file.Close()
		if err != nil {
			return nil, err
		}
		if closeErr != nil {
			return nil, closeErr
		}
	} else if err != nil {
		return nil, fmt.Errorf("读取 password.key 失败，请恢复完整的 config 备份: %w", err)
	}
	if len(key) != 32 {
		return nil, errors.New("password.key 必须是 32 字节密钥")
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	return cipher.NewGCMWithRandomNonce(block)
}

func (accounts *Accounts) encryptPassword(username, password string) string {
	encrypted := accounts.passwordCipher.Seal(nil, nil, []byte(password), []byte(username))
	return base64.StdEncoding.EncodeToString(encrypted)
}

func (accounts *Accounts) decryptPassword(username, encrypted string) (string, error) {
	content, err := base64.StdEncoding.DecodeString(encrypted)
	if err != nil {
		return "", errors.New("密码密文格式无效")
	}
	plaintext, err := accounts.passwordCipher.Open(nil, nil, content, []byte(username))
	if err != nil {
		return "", errors.New("密码解密失败")
	}
	return string(plaintext), nil
}

func (accounts *Accounts) RevealPassword(username string) (string, error) {
	accounts.mu.RLock()
	defer accounts.mu.RUnlock()
	record, exists := accounts.users[username]
	if !exists {
		return "", os.ErrNotExist
	}
	if record.User.Admin {
		return "", errors.New("管理员密码由 Compose 管理，不能在网页查看")
	}
	return accounts.decryptPassword(username, record.PasswordEncrypted)
}
