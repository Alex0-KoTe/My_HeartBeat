let logPollingInterval = null;
let statusPollingInterval = null;
let lastLogLength = 0;

// ==================== СПИСКИ СКРИПТОВ ====================
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

async function runScript(filename, btnElement) {
    btnElement.disabled = true;
    btnElement.textContent = '⏳ Выполняется...';
    updateStatus('running', 'Выполнение: ' + filename);
    
    document.getElementById('stop-btn').style.display = 'inline-block';
    
    try {
        const response = await fetch(`/api/run?file=${encodeURIComponent(filename)}`);
        const result = await response.json();

        if (response.status === 409) {
            appendLog('⚠️ Скрипт уже выполняется', 'warning');
            btnElement.disabled = false;
            btnElement.textContent = '▶ Запустить';
            updateStatus('running', 'Скрипт уже активен');
            document.getElementById('stop-btn').style.display = 'none';
        } else if (response.ok && result.status === 'success') {
            appendLog('✅ Запуск: ' + filename, 'info');
            startLogPolling();
            startStatusPolling(btnElement, filename);
        } else {
            appendLog('❌ Ошибка: ' + (result.message || 'Неизвестная ошибка'), 'error');
            btnElement.disabled = false;
            btnElement.textContent = '▶ Запустить';
            updateStatus('error', 'Ошибка запуска');
            document.getElementById('stop-btn').style.display = 'none';
        }
    } catch (error) {
        appendLog(' Ошибка сети: ' + error.message, 'error');
        btnElement.disabled = false;
        btnElement.textContent = '▶ Запустить';
        updateStatus('error', 'Ошибка сети');
        document.getElementById('stop-btn').style.display = 'none';
    }
}

// ==================== ОСТАНОВКА СКРИПТА ====================
async function stopScript() {
    try {
        const response = await fetch('/api/stop');
        const result = await response.json();
        
        if (response.ok && result.status === 'success') {
            appendLog('⏹ Скрипт остановлен пользователем', 'warning');
            updateStatus('idle', 'Остановлено пользователем');
            
            document.querySelectorAll('.btn.run').forEach(btn => {
                if (btn.id !== 'upload-btn') {
                    btn.disabled = false;
                    btn.textContent = '▶ Запустить';
                }
            });
            
            document.getElementById('stop-btn').style.display = 'none';
            
            if (statusPollingInterval) {
                clearInterval(statusPollingInterval);
                statusPollingInterval = null;
            }
        } else {
            appendLog('⚠️ Нет выполняемого скрипта', 'warning');
        }
    } catch (error) {
        appendLog('❌ Ошибка остановки: ' + error.message, 'error');
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

// ==================== ПОЛЛИНГ И ТЕРМИНАЛ ====================
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
        } catch (error) { /* Тихая ошибка */ }
    }, 300);
}

function startStatusPolling(btnElement, filename) {
    if (statusPollingInterval) clearInterval(statusPollingInterval);
    statusPollingInterval = setInterval(async () => {
        try {
            const response = await fetch('/api/status');
            const result = await response.json();
            
            if (result.status !== 'running') {
                clearInterval(statusPollingInterval);
                statusPollingInterval = null;
                btnElement.disabled = false;
                btnElement.textContent = '▶ Запустить';
                
                document.getElementById('stop-btn').style.display = 'none';
                
                if (result.status === 'idle') {
                    updateStatus('idle', 'Готов к работе');
                    appendLog('✅ Скрипт завершён: ' + filename, 'info');
                } else if (result.status === 'crashed') {
                    updateStatus('error', 'Скрипт упал с ошибкой');
                    appendLog('💥 Скрипт упал: ' + filename, 'error');
                } else if (result.status === 'watchdog_timeout') {
                    updateStatus('error', 'Превышен таймаут watchdog');
                    appendLog('⏰ Watchdog timeout: ' + filename, 'warning');
                } else if (result.status === 'memory_error') {
                    updateStatus('error', 'Недостаточно памяти');
                    appendLog('💾 Memory error: ' + filename, 'error');
                }
            }
        } catch (error) { /* Тихая ошибка */ }
    }, 1000);
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
    try {
        await fetch('/api/logs/clear');
        document.getElementById('terminal-output').innerHTML = '<div class="log-line log-system">Терминал очищен</div>';
        lastLogLength = 0;
    } catch (error) {
        appendLog('Ошибка очистки: ' + error.message, 'error');
    }
}

async function refreshLogs() {
    try {
        const response = await fetch('/api/logs');
        const logs = await response.text();
        document.getElementById('terminal-output').innerHTML = '';
        appendLog(logs, 'auto');
        lastLogLength = logs.length;
    } catch (error) {
        appendLog('Ошибка обновления: ' + error.message, 'error');
    }
}

// Запуск при загрузке страницы
loadScripts();