"use strict";

const byId = (name) => document.getElementById(name);
const state = {user:null,csrf:"",directory:[],entries:[],uploading:false,upload:null,cancelUpload:false,view:"files",requestNumber:0};
let toastTimer;
let dialogAction = null;
let dialogCancel = null;

function element(tag,text,className) {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (className) node.className = className;
  return node;
}

function toast(message,error=false) {
  clearTimeout(toastTimer);
  byId("toast").textContent = message;
  byId("toast").classList.toggle("is-error",error);
  byId("toast").hidden = false;
  toastTimer = setTimeout(() => { byId("toast").hidden = true; },5000);
}

function showLogin() {
  state.upload?.abort();
  state.user = null;
  state.csrf = "";
  state.directory = [];
  state.entries = [];
  state.requestNumber++;
  byId("app-view").hidden = true;
  byId("login-view").hidden = false;
  byId("file-list").replaceChildren();
  byId("user-list").replaceChildren();
  if (byId("edit-dialog").open) byId("edit-dialog").close();
}

async function errorMessage(response) {
  if (response.headers.get("Content-Type")?.includes("application/json")) {
    const result = await response.json().catch(() => ({}));
    if (result.error) return result.error;
  }
  const messages = {403:"没有操作权限或路径无效",404:"文件或目录不存在",405:"操作失败，请检查目录、上传大小或文件是否正被占用",409:"目标目录不存在，或当前目录非空",412:"目标文件已存在，未覆盖",413:"文件超过服务端上传大小限制",423:"文件被其他客户端锁定",507:"存储空间不足"};
  return messages[response.status] || `操作失败（${response.status}）`;
}

async function request(target,options={}) {
  const method = options.method || "GET";
  const headers = new Headers(options.headers || {});
  if (!["GET","HEAD","OPTIONS","PROPFIND"].includes(method) && state.csrf) headers.set("X-CSRF-Token",state.csrf);
  const response = await fetch(target,{...options,method,headers,credentials:"same-origin"});
  if (response.status === 401 && target !== "/api/login") { showLogin(); throw new Error("登录已失效，请重新登录"); }
  if (!response.ok) throw new Error(await errorMessage(response));
  return response;
}

async function api(target,method="GET",body) {
  const response = await request(target,{method,body:body === undefined ? undefined : JSON.stringify(body),headers:{"Content-Type":"application/json"}});
  return response.json();
}

function davPath(segments=state.directory) { return "/dav/" + segments.map(encodeURIComponent).join("/"); }
function normalizedPath(value) { return decodeURIComponent(value).replace(/\/+$/,""); }
function formatSize(value) {
  if (!Number.isFinite(value)) return "—";
  if (value < 1024) return `${value} B`;
  const units = ["KB","MB","GB","TB"];
  let size=value/1024,index=0;
  while(size>=1024 && index<units.length-1){size/=1024;index++;}
  return `${size.toFixed(size>=10 ? 0 : 1)} ${units[index]}`;
}

function validateName(value) {
  const name = value.trim();
  if (!name || name === "." || name === ".." || /[\\/:\x00]/.test(name) || name.startsWith(".maplestore-upload-")) throw new Error("请输入有效名称，不包含斜杠、反斜杠或冒号");
  return name;
}

function pathSegments(value) { return value.split("/").filter(Boolean).map(validateName); }

function updateAccount(result) {
  state.user = result.user;
  state.csrf = result.csrf;
  byId("login-view").hidden = true;
  byId("app-view").hidden = false;
  byId("account-name").textContent = state.user.username;
  byId("account-role").textContent = state.user.admin ? "管理员" : state.user.readOnly ? "只读" : "读写";
  byId("access-label").textContent = state.user.readOnly ? "只读账号 · 独立存储目录" : "独立存储目录";
  byId("users-tab").hidden = !state.user.admin;
  byId("password-button").hidden = state.user.admin;
  byId("upload-button").disabled = state.user.readOnly;
  byId("mkdir-button").disabled = state.user.readOnly;
  byId("dav-address").textContent = `${location.origin}/dav/`;
  switchView("files");
}

function switchView(view) {
  state.view = view;
  byId("files-panel").hidden = view !== "files";
  byId("users-panel").hidden = view !== "users";
  byId("files-tab").classList.toggle("active",view === "files");
  byId("users-tab").classList.toggle("active",view === "users");
  byId("section-title").textContent = view === "files" ? "我的文件" : "账号管理";
  (view === "files" ? loadFiles() : loadUsers()).catch(error => toast(error.message,true));
}

function renderBreadcrumbs() {
  const container = byId("breadcrumbs");
  container.replaceChildren();
  const home = element("button","根目录");
  home.addEventListener("click",() => navigate([]));
  container.append(home);
  state.directory.forEach((component,index) => {
    const button = element("button",component);
    button.addEventListener("click",() => navigate(state.directory.slice(0,index+1)));
    container.append(element("span","/"),button);
  });
}

function navigate(directory) {
  state.directory = [...directory];
  byId("search-input").value = "";
  loadFiles().catch(error => toast(error.message,true));
}

async function loadFiles() {
  const sequence = ++state.requestNumber;
  const currentPath = davPath();
  renderBreadcrumbs();
  state.entries = [];
  byId("file-list").replaceChildren();
  byId("file-empty").hidden = true;
  byId("file-count").textContent = "正在加载…";
  const response = await request(currentPath,{method:"PROPFIND",headers:{Depth:"1","Content-Type":"application/xml; charset=utf-8"},body:'<d:propfind xmlns:d="DAV:"><d:prop><d:displayname/><d:getcontentlength/><d:getlastmodified/><d:resourcetype/></d:prop></d:propfind>'});
  const xml = new DOMParser().parseFromString(await response.text(),"application/xml");
  if (xml.getElementsByTagName("parsererror").length) throw new Error("目录列表格式无效");
  if (sequence !== state.requestNumber || !state.user) return;
  state.entries = Array.from(xml.getElementsByTagNameNS("DAV:","response")).flatMap(item => {
    const href = item.getElementsByTagNameNS("DAV:","href")[0]?.textContent;
    if (!href) return [];
    const url = new URL(href,location.origin);
    if (url.origin !== location.origin || !url.pathname.startsWith("/dav/") || normalizedPath(url.pathname) === normalizedPath(currentPath)) return [];
    const segments = url.pathname.split("/").filter(Boolean);
    const name = decodeURIComponent(segments.at(-1));
    const property = Array.from(item.getElementsByTagNameNS("DAV:","propstat")).find(value => value.getElementsByTagNameNS("DAV:","status")[0]?.textContent.includes(" 200 "));
    if (!property) return [];
    const isDirectory = property.getElementsByTagNameNS("DAV:","collection").length > 0;
    const size = Number(property.getElementsByTagNameNS("DAV:","getcontentlength")[0]?.textContent);
    const modified = property.getElementsByTagNameNS("DAV:","getlastmodified")[0]?.textContent;
    return [{name,path:url.pathname,isDirectory,size,modified}];
  });
  state.entries.sort((first,second) => Number(second.isDirectory)-Number(first.isDirectory) || first.name.localeCompare(second.name,"zh-CN"));
  renderFiles();
}

function actionButton(label,action,danger=false) {
  const button = element("button",label,danger ? "quiet danger" : "quiet");
  button.addEventListener("click",() => {
    if (state.uploading) { toast("请等待上传完成，或先取消上传"); return; }
    Promise.resolve().then(action).catch(error => toast(error.message,true));
  });
  return button;
}

function renderFiles() {
  const filter = byId("search-input").value.toLocaleLowerCase();
  const entries = state.entries.filter(item => item.name.toLocaleLowerCase().includes(filter));
  byId("file-list").replaceChildren();
  for (const entry of entries) {
    const row = element("tr");
    const nameCell = element("td");
    const wrapper = element("div",undefined,"filename");
    wrapper.append(element("span",entry.isDirectory ? "▱" : "▤",`file-icon${entry.isDirectory ? " folder" : ""}`));
    const link = element("a",entry.name,"file-link");
    link.href = entry.path;
    link.title = entry.name;
    if (entry.isDirectory) link.addEventListener("click",event => {event.preventDefault();navigate([...state.directory,entry.name]);});
    else link.download = entry.name;
    wrapper.append(link);
    nameCell.append(wrapper);
    const date = new Date(entry.modified);
    row.append(nameCell,element("td",entry.isDirectory ? "—" : formatSize(entry.size)),element("td",Number.isNaN(date.valueOf()) ? "—" : date.toLocaleString("zh-CN",{hour12:false})));
    const actions = element("td");
    const buttons = element("div",undefined,"row-actions");
    if (!entry.isDirectory) { const download = element("a","下载");download.href=entry.path;download.download=entry.name;buttons.append(download); }
    if (!state.user.readOnly) buttons.append(actionButton("重命名",() => renameFile(entry)),actionButton("移动",() => moveFile(entry)),actionButton("删除",() => deleteFile(entry),true));
    actions.append(buttons);
    row.append(actions);
    byId("file-list").append(row);
  }
  byId("file-empty").hidden = entries.length !== 0;
  byId("file-empty").querySelector("h3").textContent = filter ? "没有匹配的文件" : "这个文件夹还是空的";
  byId("file-empty").querySelector("p").textContent = filter ? "试试其他名称。" : state.user.readOnly ? "此账号只能浏览和下载已有文件。" : "上传文件，或新建一个文件夹。";
  byId("file-count").textContent = filter ? `${entries.length} 项匹配，共 ${state.entries.length} 项` : `${entries.length} 个项目`;
}

function openDialog(title,fields,action,options={}) {
  byId("dialog-title").textContent = title;
  byId("dialog-error").textContent = "";
  byId("dialog-fields").replaceChildren();
  byId("dialog-submit").textContent = options.submit || "确定";
  byId("dialog-submit").classList.toggle("danger",!!options.danger);
  if (options.description) byId("dialog-fields").append(element("p",options.description));
  for (const field of fields) {
    const label = element("label",field.label,field.type === "checkbox" ? "checkbox" : "");
    const input = element("input");
    input.name = field.name;
    input.type = field.type || "text";
    input.value = field.value || (input.type === "checkbox" ? "on" : "");
    input.required = field.required !== false && input.type !== "checkbox";
    input.autocomplete = input.type === "password" ? "new-password" : "off";
    if (input.type === "password") input.minLength = 12;
    if (field.placeholder) input.placeholder = field.placeholder;
    label.append(input);
    byId("dialog-fields").append(label);
  }
  dialogAction = action;
  dialogCancel = options.onCancel || null;
  byId("edit-dialog").showModal();
}

function closeDialog(cancelled=true) {
  byId("edit-dialog").close();
  if (cancelled) dialogCancel?.();
  dialogCancel = null;
  dialogAction = null;
}

function mkdir() {
  openDialog("新建文件夹",[{name:"name",label:"文件夹名称"}],async form => {
    const name = validateName(form.get("name"));
    await request(davPath([...state.directory,name]),{method:"MKCOL"});
    await loadFiles();
    toast("文件夹已创建");
  });
}

function renameFile(entry) {
  openDialog("重命名",[{name:"name",label:"新名称",value:entry.name}],async form => {
    const name = validateName(form.get("name"));
    if (name === entry.name) return;
    await request(entry.path,{method:"MOVE",headers:{Destination:new URL(davPath([...state.directory,name]),location.origin).href,Overwrite:"F"}});
    await loadFiles();toast("已重命名");
  });
}

function moveFile(entry) {
  openDialog("移动文件",[{name:"directory",label:"目标目录（相对于你的根目录）",value:"/",placeholder:"例如 /documents"}],async form => {
    const destination = [...pathSegments(form.get("directory")),entry.name];
    await request(entry.path,{method:"MOVE",headers:{Destination:new URL(davPath(destination),location.origin).href,Overwrite:"F"}});
    await loadFiles();toast("已移动");
  },{description:`移动「${entry.name}」。目标文件夹须已存在；不会覆盖同名文件。`});
}

function deleteFile(entry) {
  openDialog("确认永久删除",[],async () => {
    await request(entry.path,{method:"DELETE"});await loadFiles();toast("已删除");
  },{description:`确定删除「${entry.name}」${entry.isDirectory ? "及其全部内容" : ""}？此操作没有回收站，无法在 MapleStore 中撤销。`,danger:true,submit:"永久删除"});
}

function askOverwrite(name) {
  return new Promise(resolve => openDialog("覆盖同名文件？",[],async () => resolve(true),{description:`「${name}」已存在。继续上传将替换原文件，不保留历史版本。`,submit:"覆盖上传",onCancel:()=>resolve(false)}));
}

function uploadFile(file,target,index,total) {
  return new Promise((resolve,reject) => {
    const upload = new XMLHttpRequest();
    state.upload = upload;
    upload.open("PUT",target);
    upload.withCredentials = true;
    upload.setRequestHeader("X-CSRF-Token",state.csrf);
    upload.upload.onprogress = event => { if (event.lengthComputable) byId("progress-bar").value = event.loaded/event.total*100; };
    upload.onload = () => {
      if (upload.status>=200 && upload.status<300) resolve();
      else { if (upload.status === 401) showLogin();reject(new Error(`上传失败（${upload.status}）：${file.name}`)); }
    };
    upload.onerror = () => reject(new Error("网络中断，上传未完成"));
    upload.onabort = () => reject(new Error("上传已取消"));
    byId("upload-label").textContent = `正在上传 ${index}/${total} · ${file.name}`;
    byId("progress-bar").value = 0;
    upload.send(file);
  });
}

async function uploadFiles(files) {
  if (!files.length || state.uploading) return;
  const directory = [...state.directory];
  const existing = new Set(state.entries.map(entry=>entry.name));
  state.uploading = true;state.cancelUpload = false;
  byId("upload-button").disabled = true;byId("mkdir-button").disabled = true;
  byId("upload-progress").hidden = false;
  let completed = 0;
  try {
    for (const [index,file] of files.entries()) {
      if (state.cancelUpload) break;
      validateName(file.name);
      if (existing.has(file.name) && !await askOverwrite(file.name)) continue;
      if (state.cancelUpload) break;
      await uploadFile(file,davPath([...directory,file.name]),index+1,files.length);
      completed++;
    }
    toast(`已上传 ${completed} 个文件`);
  } catch (error) { toast(error.message,true); }
  finally {
    state.uploading = false;state.upload = null;
    byId("upload-progress").hidden = true;
    byId("upload-button").disabled = !!state.user?.readOnly;
    byId("mkdir-button").disabled = !!state.user?.readOnly;
    byId("upload-input").value = "";
    if (state.user) loadFiles().catch(error=>toast(error.message,true));
  }
}

async function loadUsers() {
  const users = await api("/api/users");
  if (!state.user?.admin) return;
  byId("user-list").replaceChildren();
  for (const user of users) {
    const row = element("tr");
    const name = element("td",user.username);
    name.append(element("code",`./data/${user.username}/`,"account-directory"));
    row.append(name,element("td",user.admin ? "管理员" : user.readOnly ? "只读" : "读写"));
    const status = element("td");status.append(element("span",user.disabled ? "已停用" : "正常",`tag${user.disabled ? " disabled" : ""}`));row.append(status);
    const actions = element("td");
    if (user.admin) actions.append(element("span","由 Compose 管理","muted"));
    else {
      const wrapper=element("div",undefined,"row-actions");
      wrapper.append(actionButton("重置密码",()=>openDialog(`重置 ${user.username} 的密码`,[{name:"password",label:"新密码（至少 12 字节）",type:"password"}],async form=>{await api(`/api/users/${user.username}`,"PATCH",{password:form.get("password")});toast("密码已重置，原有会话已失效");})),actionButton(user.readOnly ? "设为读写" : "设为只读",async()=>{await api(`/api/users/${user.username}`,"PATCH",{readOnly:!user.readOnly});await loadUsers();toast("权限已更新");}),actionButton(user.disabled ? "启用" : "停用",async()=>{await api(`/api/users/${user.username}`,"PATCH",{disabled:!user.disabled});await loadUsers();toast(user.disabled ? "账号已启用" : "账号已停用，文件保留");},!user.disabled));
      actions.append(wrapper);
    }
    row.append(actions);byId("user-list").append(row);
  }
}

byId("login-form").addEventListener("submit",async event=>{
  event.preventDefault();const button=event.target.querySelector("button[type=submit]");button.disabled=true;byId("login-error").textContent="";
  try {const form=new FormData(event.target);const result=await api("/api/login","POST",{username:form.get("username").trim(),password:form.get("password")});event.target.reset();updateAccount(result);}
  catch(error){byId("login-error").textContent=error.message;}
  finally{button.disabled=false;}
});
byId("edit-form").addEventListener("submit",async event=>{
  event.preventDefault();byId("dialog-submit").disabled=true;byId("dialog-error").textContent="";
  try {await dialogAction?.(new FormData(event.target));closeDialog(false);}
  catch(error){byId("dialog-error").textContent=error.message;}
  finally{byId("dialog-submit").disabled=false;}
});
byId("dialog-close").addEventListener("click",()=>closeDialog());
byId("dialog-cancel").addEventListener("click",()=>closeDialog());
byId("edit-dialog").addEventListener("cancel",event=>{event.preventDefault();closeDialog();});
byId("files-tab").addEventListener("click",()=>switchView("files"));
byId("users-tab").addEventListener("click",()=>switchView("users"));
byId("refresh-button").addEventListener("click",()=>loadFiles().catch(error=>toast(error.message,true)));
byId("search-input").addEventListener("input",renderFiles);
byId("mkdir-button").addEventListener("click",mkdir);
byId("upload-button").addEventListener("click",()=>byId("upload-input").click());
byId("upload-input").addEventListener("change",event=>uploadFiles(Array.from(event.target.files)));
byId("cancel-upload").addEventListener("click",()=>{state.cancelUpload=true;state.upload?.abort();});
byId("logout-button").addEventListener("click",async()=>{try{await api("/api/logout","POST",{});showLogin();}catch(error){toast(error.message,true);}});
byId("copy-address").addEventListener("click",async()=>{try{await navigator.clipboard.writeText(byId("dav-address").textContent);toast("WebDAV 地址已复制");}catch{toast("浏览器不允许复制，请手动选择地址复制",true);}});
byId("add-user-button").addEventListener("click",()=>openDialog("新建独立账号",[{name:"username",label:"账号（3–32 位小写字母、数字、_ 或 -）"},{name:"password",label:"初始密码（至少 12 字节）",type:"password"},{name:"readOnly",label:"只读账号：只允许浏览和下载",type:"checkbox"}],async form=>{await api("/api/users","POST",{username:form.get("username").trim(),password:form.get("password"),readOnly:form.get("readOnly")==="on"});await loadUsers();toast("账号和独立目录已创建");}));
byId("password-button").addEventListener("click",()=>openDialog("修改密码",[{name:"currentPassword",label:"当前密码",type:"password"},{name:"password",label:"新密码（至少 12 字节）",type:"password"}],async form=>{await api("/api/password","POST",{currentPassword:form.get("currentPassword"),password:form.get("password")});showLogin();toast("密码已修改，请重新登录");}));

api("/api/me").then(updateAccount).catch(()=>showLogin());
