let ws = null;
let reconnectTimer = null;
let currentRunningFile = null;

// ==================== WEBSOCKET ====================
function connectWebSocket() {
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
        return;
    }
    
    const wsUrl = `ws://${window.location.hostname}/ws`;
    console.log('Connecting to WebSocket:', wsUrl);
    
    ws = new WebSocket(wsUrl);
    
    ws.onopen = () => {
        console.log('WebSocket connected');
        appendLog('🔌 WebSocket подключён', 'system');
        
        // Запрашиваем список скриптов и текущий статус
        loadScripts();
        loadCurrentStatus();
    };
    
    ws.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);
            
            if (data.type === 'log') {
                appendLog(data.data, 'auto');
            }
            else if (data.type === 'status') {
                handleStatusUpdate(data);
            }
            else if (data.type === 'error') {
                appendLog('❌ ' + (data.message || 'Ошибка'), 'error');
            }
        } catch (e) {
            console.error('Failed to parse WebSocket message:', e);
        }
    };
    
    ws.onclose = () => {
        console.log('WebSocket disconnected');
        appendLog('️ WebSocket отключён, переподключение...', 'warning');
        
        if (reconnectTimer) clearTimeout(reconnectTimer);
        reconnectTimer = setTimeout(connectWebSocket, 3000);
    };
    
    ws.onerror = (error) => {
        console.error('WebSocket error:', error);
    };
}

function sendWebSocketCommand(cmd) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(cmd);
    } else {
        appendLog('❌ WebSocket не подключён', 'error');
    }
}

// ==================== УПРАВЛЕНИЕ СКРИПТАМИ ====================
async function loadScripts() {
    try {
        const response = await fetch('/api/scripts');
        if (!response.ok) throw new Error('Network response was not ok');
        const scripts = await response.json();
        
        const listDiv = document.getElementById('script-list');
        listDiv.innerHTML = '';
        
        if (scripts.length === 0) {
            listDiv.innerHTML = '<p style="text-align:center; color:#888;">Папка /scripts пуста</p>';
            return;
        }

        scripts.forEach(file => {
            const item = document.createElement('div');
            item.className = 'script-item';
            
            const nameSpan = document.createElement('span');
            nameSpan.textContent = '📜 ' + file;
            
            const btn = document.createElement('button');
            btn.className = 'btn run';
            btn.textContent = '▶ Запустить';
            btn.id = 'btn-' + file;
            btn.onclick = () => runScript(file, btn);
            
            item.appendChild(nameSpan);
            item.appendChild(btn);
            listDiv.appendChild(item);
        });
    } catch (error) {
        document.getElementById('script-list').innerHTML = 
            '<p style="color:#ff5555; text-align:center;">Ошибка загрузки: ' + error.message + '</p>';
    }
}

function runScript(filename, btnElement) {
    currentRunningFile = filename;
    btnElement.disabled = true;
    btnElement.textContent = '⏳ Выполняется...';
    updateStatus('running', 'Выполнение: ' + filename);
    document.getElementById('stop-btn').style.display = 'inline-block';
    
    sendWebSocketCommand('run:' + filename);
}

async function stopScript() {
    sendWebSocketCommand('stop');
}

// Обработка обновления статуса
function handleStatusUpdate(data) {
    if (data.state === 'idle') {
        updateStatus('idle', 'Готов к работе');
        appendLog('✅ ' + (data.message || 'Скрипт завершён'), 'info');
        
        // Разблокировать все кнопки запуска
        document.querySelectorAll('.btn.run').forEach(btn => {
            if (btn.id !== 'upload-btn') {
                btn.disabled = false;
                btn.textContent = '▶ Запустить';
            }
        });
        document.getElementById('stop-btn').style.display = 'none';
        currentRunningFile = null;
    }
    else if (data.state === 'watchdog_timeout') {
        updateStatus('error', 'Превышен таймаут watchdog');
        appendLog(' ' + (data.message || 'Watchdog timeout'), 'warning');
        
        document.querySelectorAll('.btn.run').forEach(btn => {
            if (btn.id !== 'upload-btn') {
                btn.disabled = false;
                btn.textContent = '▶ Запустить';
            }
        });
        document.getElementById('stop-btn').style.display = 'none';
        currentRunningFile = null;
    }
    else if (data.state === 'crashed') {
        updateStatus('error', 'Скрипт упал с ошибкой');
        appendLog('💥 ' + (data.message || 'Script crashed'), 'error');
        
        document.querySelectorAll('.btn.run').forEach(btn => {
            if (btn.id !== 'upload-btn') {
                btn.disabled = false;
                btn.textContent = '▶ Запустить';
            }
        });
        document.getElementById('stop-btn').style.display = 'none';
        currentRunningFile = null;
    }
    else if (data.state === 'memory_error') {
        updateStatus('error', 'Недостаточно памяти');
        appendLog('💾 ' + (data.message || 'Memory error'), 'error');
    }
}

// Загрузка текущего статуса при подключении
async function loadCurrentStatus() {
    try {
        const response = await fetch('/api/status');
        const result = await response.json();
        
        if (result.status === 'running') {
            updateStatus('running', 'Выполняется скрипт');
            document.getElementById('stop-btn').style.display = 'inline-block';
        } else {
            updateStatus('idle', 'Готов к работе');
        }
    } catch (error) {
        console.error('Failed to load status:', error);
    }
}

// ==================== ЗАГРУЗКА ФАЙЛОВ ====================
document.getElementById('file-input').addEventListener('change', function(e) {
    const file = e.target.files[0];
    if (file) {
        document.getElementById('file-name').textContent = file.name + ' (' + file.size + ' bytes)';
        document.getElementById('upload-btn').disabled = false;
        document.getElementById('upload-status').style.display = 'none';
    }
});

async function uploadFile() {
    const fileInput = document.getElementById('file-input');
    const file = fileInput.files[0];
    
    if (!file) {
        showUploadStatus('Выберите файл', 'error');
        return;
    }
    
    const uploadBtn = document.getElementById('upload-btn');
    uploadBtn.disabled = true;
    uploadBtn.textContent = '⏳ Загрузка...';
    
    try {
        const response = await fetch(`/api/upload?filename=${encodeURIComponent(file.name)}`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/octet-stream' },
            body: file
        });
        
        const result = await response.json();
        
        if (response.ok && result.status === 'success') {
            showUploadStatus('✅ Файл загружен (' + result.size + ' bytes)', 'success');
            fileInput.value = '';
            document.getElementById('file-name').textContent = 'Файл не выбран';
            document.getElementById('upload-btn').disabled = true;
            loadScripts();
        } else {
            showUploadStatus('❌ Ошибка: ' + (result.message || 'Неизвестная ошибка'), 'error');
        }
    } catch (error) {
        showUploadStatus('❌ Ошибка сети: ' + error.message, 'error');
    } finally {
        uploadBtn.disabled = false;
        uploadBtn.textContent = '⬆️ Загрузить';
    }
}

function showUploadStatus(message, type) {
    const statusDiv = document.getElementById('upload-status');
    statusDiv.textContent = message;
    statusDiv.className = type;
    statusDiv.style.display = 'block';
    setTimeout(() => { statusDiv.style.display = 'none'; }, 5000);
}

// ==================== ТЕРМИНАЛ ====================
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
            div.className = 'log-line log-' + finalType;
            div.textContent = line;
            terminal.appendChild(div);
        }
    });
    terminal.scrollTop = terminal.scrollHeight;
}

function updateStatus(state, text) {
    document.getElementById('status-dot').className = 'status-dot ' + state;
    document.getElementById('status-text').textContent = text;
}

async function clearTerminal() {
    sendWebSocketCommand('clear_logs');
    document.getElementById('terminal-output').innerHTML = '<div class="log-line log-system">Терминал очищен</div>';
}

async function refreshLogs() {
    appendLog('🔄 Обновление не требуется (WebSocket активен)', 'system');
}

// Запуск при загрузке страницы
connectWebSocket();