// Глобальные переменные
let ws = null;
let reconnectTimer = null;
let activeFile = null;
let files = [];
let autoScroll = true;

// ==================== ИНИЦИАЛИЗАЦИЯ ====================
document.addEventListener('DOMContentLoaded', () => {
    initTabs();
    initWebSocket();
    initFileList();
    initTerminal();
    initMonitor();
    initSettings();
    initDocs();
});

// ==================== ВКЛАДКИ ====================
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
    // Скрыть все вкладки
    document.querySelectorAll('.tab-content').forEach(tab => {
        tab.classList.remove('active');
    });
    document.querySelectorAll('.tab-btn').forEach(btn => {
        btn.classList.remove('active');
    });
    
    // Показать выбранную
    document.getElementById(`tab-${tabId}`).classList.add('active');
    document.querySelector(`[data-tab="${tabId}"]`).classList.add('active');
    
    // Специфичная логика для вкладок
    if (tabId === 'editor' && activeFile) {
        loadFileToEditor(activeFile);
    }
}

// ==================== WEBSOCKET ====================
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
        try {
            const data = JSON.parse(event.data);
            handleWSMessage(data);
        } catch (e) {
            console.error('Failed to parse WS message:', e);
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

// ==================== ФАЙЛЫ ====================
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
                // Снять галочки с других
                document.querySelectorAll('.file-item input[type="checkbox"]').forEach(cb => {
                    if (cb !== checkbox) cb.checked = false;
                });
                activeFile = file;
                document.getElementById('active-file-label').textContent = `Файл: ${file}`;
            } else {
                activeFile = null;
                document.getElementById('active-file-label').textContent = 'Файл не выбран';
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
    try {
        const response = await fetch(`/api/upload?filename=${encodeURIComponent(file.name)}`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/octet-stream' },
            body: file
        });
        
        if (response.ok) {
            appendLog(`✅ Файл загружен: ${file.name}`, 'info');
            loadFiles();
        } else {
            appendLog(`❌ Ошибка загрузки: ${file.name}`, 'error');
        }
    } catch (error) {
        appendLog(`❌ Ошибка сети: ${error.message}`, 'error');
    }
}

function confirmDelete(filename) {
    showModal(`Удалить файл ${filename}?`, async () => {
        // TODO: API удаления файла
        appendLog(`🗑️ Файл удалён: ${filename}`, 'info');
        loadFiles();
    });
}

// ==================== ТЕРМИНАЛ ====================
function initTerminal() {
    document.getElementById('btn-clear').addEventListener('click', () => {
        document.getElementById('terminal-output').innerHTML = '';
        sendWS('clear_logs');
    });
    
    document.getElementById('btn-autoscroll').addEventListener('click', (e) => {
        autoScroll = !autoScroll;
        e.target.classList.toggle('active', autoScroll);
    });
    
    document.getElementById('btn-run').addEventListener('click', () => {
        if (activeFile) {
            sendWS(`run:${activeFile}`);
        } else {
            appendLog('⚠️ Выберите файл', 'warning');
        }
    });
    
    document.getElementById('btn-stop').addEventListener('click', () => {
        sendWS('stop');
    });
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

// ==================== МОНИТОР ====================
function initMonitor() {
    // Генерация индикаторов реле
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
    
    // Генерация кнопок входов
    const inputGrid = document.getElementById('input-buttons');
    for (let i = 0; i < 8; i++) {
        const item = document.createElement('div');
        item.className = 'io-item';
        item.innerHTML = `
            <button class="io-button" id="input-${i}" data-index="${i}">IN${i}</button>
        `;
        inputGrid.appendChild(item);
    }
    
    // Обработчики кнопок входов
    document.querySelectorAll('.io-button').forEach(btn => {
        btn.addEventListener('click', () => {
            const index = btn.dataset.index;
            btn.classList.toggle('active');
            // TODO: Отправить команду на ESP32
            appendLog(` Вход IN${index} переключён`, 'info');
        });
    });
    
    document.getElementById('btn-reset-io').addEventListener('click', () => {
        document.querySelectorAll('.io-button').forEach(btn => {
            btn.classList.remove('active');
        });
        appendLog('🔄 Ручное управление сброшено', 'info');
    });
}

// ==================== НАСТРОЙКИ ====================
function initSettings() {
    document.getElementById('btn-save-wifi').addEventListener('click', () => {
        appendLog('💾 Wi-Fi настройки сохранены (TODO)', 'info');
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

// ==================== ДОКУМЕНТАЦИЯ ====================
function initDocs() {
    document.getElementById('btn-copy-prompt').addEventListener('click', () => {
        const promptText = document.getElementById('ai-prompt').value;
        navigator.clipboard.writeText(promptText).then(() => {
            appendLog('📋 Промт скопирован в буфер обмена', 'info');
        });
    });
}

// ==================== МОДАЛЬНОЕ ОКНО ====================
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

// ==================== ЗАГЛУШКИ ====================
function loadFileToEditor(filename) {
    // TODO: Загрузить содержимое файла в CodeMirror
    appendLog(`📝 Загрузка файла ${filename} в редактор (TODO)`, 'info');
}