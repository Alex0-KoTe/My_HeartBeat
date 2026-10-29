// ============================================================================
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ============================================================================
let ws = null;
let reconnectTimer = null;
let activeFile = null;
let files = [];
let autoScroll = true;

// Переменные для CodeMirror
let editor = null;
let isCodeMirrorLoading = false;
let isCodeMirrorLoaded = false;

// Переменные для polling логов
let lastLogLength = 0;
let logPollingInterval = null;

// ============================================================================
// ИНИЦИАЛИЗАЦИЯ ПРИ ЗАГРУЗКЕ СТРАНИЦЫ
// ============================================================================
document.addEventListener('DOMContentLoaded', () => {
    // Читаем сохранённые состояния из LocalStorage
    const savedTab = localStorage.getItem('activeTab') || 'workspace';
    const savedFile = localStorage.getItem('activeFile');

    if (savedFile) {
        activeFile = savedFile;
    }

    // Инициализируем модули
    initTabs();
    
    // Переключаемся на сохранённую вкладку
    switchTab(savedTab);
    
    initWebSocket();
    initFileList();
    initTerminal();
    initMonitor();
    initSettings();
    initDocs();
});

// ============================================================================
// ВКЛАДКИ
// ============================================================================
function initTabs() {
    const tabBtns = document.querySelectorAll('.tab-btn');
    tabBtns.forEach(btn => {
        btn.addEventListener('click', () => {
            const tabId = btn.dataset.tab;
            switchTab(tabId);
        });
    });
}

function switchTab(tabId) {
    // Скрыть все вкладки и деактивировать кнопки
    document.querySelectorAll('.tab-content').forEach(tab => {
        tab.classList.remove('active');
    });
    document.querySelectorAll('.tab-btn').forEach(btn => {
        btn.classList.remove('active');
    });
    
    // Показать выбранную
    const targetTab = document.getElementById(`tab-${tabId}`);
    const targetBtn = document.querySelector(`[data-tab="${tabId}"]`);
    
    if (targetTab) targetTab.classList.add('active');
    if (targetBtn) targetBtn.classList.add('active');
    
    // Сохраняем выбор в LocalStorage
    localStorage.setItem('activeTab', tabId);
    
    // Логика для вкладки Редактора
    if (tabId === 'editor') {
        const container = document.getElementById('editor-container');
        
        if (!isCodeMirrorLoaded && !isCodeMirrorLoading) {
            container.innerHTML = '<div class="editor-placeholder">⏳ Загрузка редактора кода...</div>';
        }

        loadCodeMirrorAssets()
            .then(() => {
                initCodeMirrorEditor();
                if (editor) {
                    setTimeout(() => editor.refresh(), 100);
                }
            })
            .catch(err => {
                console.error('Ошибка загрузки CodeMirror:', err);
                container.innerHTML = '<div class="editor-placeholder" style="color:red">❌ Ошибка загрузки редактора</div>';
            });
    }
}

// ============================================================================
// ЛЕНИВАЯ ЗАГРУЗКА CODEMIRROR 5 (с темой Monokai)
// ============================================================================
function loadCodeMirrorAssets() {
    return new Promise((resolve, reject) => {
        if (isCodeMirrorLoaded) {
            resolve();
            return;
        }
        if (isCodeMirrorLoading) {
            const interval = setInterval(() => {
                if (isCodeMirrorLoaded) {
                    clearInterval(interval);
                    resolve();
                }
            }, 100);
            return;
        }

        isCodeMirrorLoading = true;

        // Загружаем CSS (базовый + тема Monokai)
        const cssLink = document.createElement('link');
        cssLink.rel = 'stylesheet';
        cssLink.href = '/cm/core.css';
        document.head.appendChild(cssLink);

        const themeLink = document.createElement('link');
        themeLink.rel = 'stylesheet';
        themeLink.href = '/cm/monokai.css';
        document.head.appendChild(themeLink);

        // Загружаем JS файлы
        const scripts = [
            '/cm/core.js',
            '/cm/lua.js',
            '/cm/brackets.js'
        ];

        let loadedCount = 0;
        scripts.forEach(src => {
            const script = document.createElement('script');
            script.src = src;
            script.onload = () => {
                loadedCount++;
                if (loadedCount === scripts.length) {
                    isCodeMirrorLoaded = true;
                    isCodeMirrorLoading = false;
                    resolve();
                }
            };
            script.onerror = () => {
                isCodeMirrorLoading = false;
                reject(new Error(`Failed to load ${src}`));
            };
            document.body.appendChild(script);
        });
    });
}

function initCodeMirrorEditor() {
    if (editor) return;

    const container = document.getElementById('editor-container');
    container.innerHTML = '';

    const textarea = document.createElement('textarea');
    textarea.id = 'code-editor-area';
    container.appendChild(textarea);

    editor = CodeMirror.fromTextArea(textarea, {
        mode: 'lua',
        lineNumbers: true,
        autoCloseBrackets: true,
        indentUnit: 4,
        tabSize: 4,
        lineWrapping: true,
        theme: 'monokai'  // Тёмная тема Monokai
    });

    editor.setValue('-- Редактор готов к работе!\n-- Выберите файл во вкладке "Рабочая область"\n\nlocal x = 42\nprint("Hello from Lua!")');
}

// ============================================================================
// WEBSOCKET
// ============================================================================
function initWebSocket() {
    connectWebSocket();
}

function connectWebSocket() {
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
        return;
    }
    
    const wsUrl = `ws://${window.location.hostname}/ws`;
    ws = new WebSocket(wsUrl);
    
    ws.onopen = () => {
        console.log('WebSocket connected');
        document.getElementById('ws-status').textContent = '🟢 Подключено';
        document.getElementById('ws-status').classList.add('connected');
        loadFiles();
    };
    
    ws.onmessage = (event) => {
        console.log('📩 Получено сообщение:', event.data);
        try {
            const data = JSON.parse(event.data);
            console.log('✅ Распарсено:', data);
            handleWSMessage(data);
        } catch (e) {
            console.error('❌ Ошибка парсинга:', e, 'Данные:', event.data);
        }
    };
    
    ws.onclose = () => {
        console.log('WebSocket disconnected');
        document.getElementById('ws-status').textContent = '🔴 Отключено';
        document.getElementById('ws-status').classList.remove('connected');
        
        if (reconnectTimer) clearTimeout(reconnectTimer);
        reconnectTimer = setTimeout(connectWebSocket, 3000);
    };
    
    ws.onerror = (error) => {
        console.error('WebSocket error:', error);
    };
}

function handleWSMessage(data) {
    if (data.type === 'log') {
        appendLog(data.data, 'auto');
    }
    else if (data.type === 'status') {
        handleStatusUpdate(data);
    }
}

function sendWS(cmd) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(cmd);
    }
}

// ============================================================================
// ФАЙЛЫ
// ============================================================================
function initFileList() {
    document.getElementById('btn-upload').addEventListener('click', () => {
        document.getElementById('file-input').click();
    });
    
    document.getElementById('file-input').addEventListener('change', (e) => {
        const file = e.target.files[0];
        if (file) uploadFile(file);
    });
}

async function loadFiles() {
    try {
        const response = await fetch('/api/scripts');
        files = await response.json();
        renderFileList();
    } catch (error) {
        console.error('Failed to load files:', error);
    }
}

function renderFileList() {
    const listDiv = document.getElementById('file-list');
    listDiv.innerHTML = '';
    
    if (files.length === 0) {
        listDiv.innerHTML = '<p class="placeholder">Нет файлов</p>';
        return;
    }
    
    files.forEach(file => {
        const item = document.createElement('div');
        item.className = 'file-item';
        
        const checkbox = document.createElement('input');
        checkbox.type = 'checkbox';
        checkbox.checked = (file === activeFile);
        checkbox.addEventListener('change', () => {
            if (checkbox.checked) {
                document.querySelectorAll('.file-item input[type="checkbox"]').forEach(cb => {
                    if (cb !== checkbox) cb.checked = false;
                });
                activeFile = file;
                document.getElementById('active-file-label').textContent = `Файл: ${file}`;
                localStorage.setItem('activeFile', file);
            } else {
                activeFile = null;
                document.getElementById('active-file-label').textContent = 'Файл не выбран';
                localStorage.removeItem('activeFile');
            }
        });
        
        const nameSpan = document.createElement('span');
        nameSpan.className = 'file-name';
        nameSpan.textContent = file;
        
        const deleteBtn = document.createElement('button');
        deleteBtn.className = 'btn-delete';
        deleteBtn.textContent = '🗑️';
        deleteBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            confirmDelete(file);
        });
        
        item.appendChild(checkbox);
        item.appendChild(nameSpan);
        item.appendChild(deleteBtn);
        listDiv.appendChild(item);
    });
}

async function uploadFile(file) {
    if (!file.name.endsWith('.lua')) {
        appendLog(`❌ Файл ${file.name} не является Lua-скриптом (.lua)`, 'error');
        document.getElementById('file-input').value = '';
        return;
    }
    
    try {
        const response = await fetch(`/api/upload?filename=${encodeURIComponent(file.name)}`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/octet-stream' },
            body: file
        });
        
        const result = await response.json();
        
        if (response.ok && result.status === 'success') {
            appendLog(`✅ Файл загружен: ${file.name} (${result.size} bytes)`, 'info');
            loadFiles();
        } else {
            appendLog(`❌ Ошибка загрузки: ${result.message || 'Unknown error'}`, 'error');
        }
    } catch (error) {
        appendLog(`❌ Ошибка сети: ${error.message}`, 'error');
    } finally {
        document.getElementById('file-input').value = '';
    }
}

function confirmDelete(filename) {
    showModal(`Удалить файл ${filename}? Это действие нельзя отменить.`, async () => {
        try {
            const response = await fetch(`/api/delete?filename=${encodeURIComponent(filename)}`, {
                method: 'POST'
            });
            
            const result = await response.json();
            
            if (response.ok && result.status === 'success') {
                appendLog(`🗑️ Файл удалён: ${filename}`, 'info');
                
                if (activeFile === filename) {
                    activeFile = null;
                    localStorage.removeItem('activeFile');
                    document.getElementById('active-file-label').textContent = 'Файл не выбран';
                }
                
                loadFiles();
            } else {
                appendLog(`❌ Ошибка удаления: ${result.message}`, 'error');
            }
        } catch (error) {
            appendLog(`❌ Ошибка сети при удалении: ${error.message}`, 'error');
        }
    });
}

// ============================================================================
// ТЕРМИНАЛ
// ============================================================================
function initTerminal() {
    document.getElementById('btn-clear').addEventListener('click', async () => {
        document.getElementById('terminal-output').innerHTML = '<div class="log-line log-system">Терминал очищен</div>';
        lastLogLength = 0;
        try {
            await fetch('/api/logs/clear');
        } catch (error) {
            console.error('Failed to clear logs:', error);
        }
    });
    
    document.getElementById('btn-autoscroll').addEventListener('click', (e) => {
        autoScroll = !autoScroll;
        e.target.classList.toggle('active', autoScroll);
    });
    
    document.getElementById('btn-run').addEventListener('click', async () => {
        if (activeFile) {
            try {
                const response = await fetch(`/api/run?file=${encodeURIComponent(activeFile)}`);
                const result = await response.json();
                
                if (response.ok && result.status === 'success') {
                    appendLog(`✅ Запуск: ${activeFile}`, 'info');
                    startLogPolling();
                } else {
                    appendLog(`❌ Ошибка запуска: ${result.message || 'Unknown'}`, 'error');
                }
            } catch (error) {
                appendLog(`❌ Ошибка сети: ${error.message}`, 'error');
            }
        } else {
            appendLog('⚠️ Выберите файл', 'warning');
        }
    });
    
    document.getElementById('btn-stop').addEventListener('click', async () => {
        try {
            const response = await fetch('/api/stop');
            const result = await response.json();
            
            if (response.ok) {
                appendLog('⏹ Скрипт остановлен', 'info');
            } else {
                appendLog('⚠️ Нет активного скрипта', 'warning');
            }
        } catch (error) {
            appendLog(`❌ Ошибка остановки: ${error.message}`, 'error');
        }
    });
    
    startLogPolling();
}

function startLogPolling() {
    if (logPollingInterval) clearInterval(logPollingInterval);
    
    logPollingInterval = setInterval(async () => {
        try {
            const response = await fetch('/api/logs');
            const logs = await response.text();
            
            if (logs.length > lastLogLength) {
                const newLogs = logs.substring(lastLogLength);
                appendLog(newLogs, 'auto');
                lastLogLength = logs.length;
            }
        } catch (error) {
            // Тихая ошибка
        }
    }, 300);
}

function appendLog(text, type = 'info') {
    const terminal = document.getElementById('terminal-output');
    const lines = text.split('\n');
    lines.forEach(line => {
        if (line.trim()) {
            const div = document.createElement('div');
            let finalType = type;
            if (type === 'auto') {
                if (line.startsWith('[TASK]') || line.startsWith('[MGR]') || line.startsWith('[WDT]')) {
                    finalType = 'info';
                } else {
                    finalType = 'lua';
                }
            }
            div.className = `log-line log-${finalType}`;
            div.textContent = line;
            terminal.appendChild(div);
        }
    });
    if (autoScroll) {
        terminal.scrollTop = terminal.scrollHeight;
    }
}

function handleStatusUpdate(data) {
    if (data.state === 'idle') {
        document.getElementById('task-status').textContent = 'Готов к работе';
    } else if (data.state === 'running') {
        document.getElementById('task-status').textContent = `Выполняется: ${data.message || ''}`;
    }
}

// ============================================================================
// МОНИТОР
// ============================================================================
function initMonitor() {
    const relayGrid = document.getElementById('relay-indicators');
    for (let i = 0; i < 8; i++) {
        const item = document.createElement('div');
        item.className = 'io-item';
        item.innerHTML = `
            <div class="io-indicator" id="relay-${i}"></div>
            <div>RL${i}</div>
        `;
        relayGrid.appendChild(item);
    }
    
    const inputGrid = document.getElementById('input-buttons');
    for (let i = 0; i < 8; i++) {
        const item = document.createElement('div');
        item.className = 'io-item';
        item.innerHTML = `
            <button class="io-button" id="input-${i}" data-index="${i}">IN${i}</button>
        `;
        inputGrid.appendChild(item);
    }
    
    document.querySelectorAll('.io-button').forEach(btn => {
        btn.addEventListener('click', () => {
            const index = btn.dataset.index;
            btn.classList.toggle('active');
            appendLog(`🔘 Вход IN${index} переключён`, 'info');
        });
    });
    
    document.getElementById('btn-reset-io').addEventListener('click', () => {
        document.querySelectorAll('.io-button').forEach(btn => {
            btn.classList.remove('active');
        });
        appendLog('🔄 Ручное управление сброшено', 'info');
    });
}

// ============================================================================
// НАСТРОЙКИ
// ============================================================================
function initSettings() {
    document.getElementById('btn-save-wifi').addEventListener('click', () => {
        appendLog('📡 Wi-Fi настройки сохранены (TODO)', 'info');
    });
    
    document.getElementById('btn-format-spiffs').addEventListener('click', () => {
        showModal('Форматировать SPIFFS? Все файлы будут удалены!', () => {
            appendLog('🗑️ SPIFFS форматирование (TODO)', 'warning');
        });
    });
    
    document.getElementById('btn-reboot').addEventListener('click', () => {
        showModal('Перезагрузить контроллер?', () => {
            appendLog('🔁 Перезагрузка (TODO)', 'warning');
        });
    });
}

// ============================================================================
// ДОКУМЕНТАЦИЯ
// ============================================================================
function initDocs() {
    document.getElementById('btn-copy-prompt').addEventListener('click', () => {
        const promptText = document.getElementById('ai-prompt').value;
        
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(promptText).then(() => {
                appendLog('📋 Промт скопирован в буфер обмена', 'info');
            }).catch(err => {
                console.error('Clipboard API failed:', err);
                fallbackCopy(promptText);
            });
        } else {
            fallbackCopy(promptText);
        }
    });
}

function fallbackCopy(text) {
    const textArea = document.createElement('textarea');
    textArea.value = text;
    textArea.style.position = 'fixed';
    textArea.style.left = '-9999px';
    document.body.appendChild(textArea);
    textArea.select();
    
    try {
        document.execCommand('copy');
        appendLog('📋 Промт скопирован в буфер обмена', 'info');
    } catch (err) {
        appendLog('❌ Не удалось скопировать', 'error');
    }
    
    document.body.removeChild(textArea);
}

// ============================================================================
// МОДАЛЬНОЕ ОКНО
// ============================================================================
function showModal(message, onConfirm) {
    const modal = document.getElementById('modal-confirm');
    document.getElementById('modal-message').textContent = message;
    modal.classList.remove('hidden');
    
    document.getElementById('modal-ok').onclick = () => {
        modal.classList.add('hidden');
        if (onConfirm) onConfirm();
    };
    
    document.getElementById('modal-cancel').onclick = () => {
        modal.classList.add('hidden');
    };
}

// ============================================================================
// ЗАГЛУШКА ДЛЯ ЗАГРУЗКИ ФАЙЛА В РЕДАКТОР
// ============================================================================
function loadFileToEditor(filename) {
    appendLog(`📝 Загрузка файла ${filename} в редактор (TODO)`, 'info');
}