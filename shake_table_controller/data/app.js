(() => {
    const el = id => document.getElementById(id);
    const qs = sel => document.querySelector(sel);
    const qsa = sel => document.querySelectorAll(sel);

    let ws = null;
    let state = {};
    let timerRunning = false;
    let timerElapsed = 0;
    let timerOrigin = 0;
    let lapCount = 0;
    let currentRole = null;
    let reconnectAttempt = 0;
    const MAX_RECONNECT = 10;
    let confirmCallback = null;
    let editingPresetId = null;
    let settingsUnlocked = false;
    let lastState = null;

    let pendingCommands = {};
    let localEvents = [];
    let localEventCounter = 0;
    let initialSyncDone = false;

    const STORAGE_KEY = 'shakeTable_session';
    const THEME_STORAGE_KEY = 'shakeTable_theme';
    const ROLE_SESSION_KEY = 'shakeTable_roleSession';
    // sessionStorage (not localStorage): survives a refresh but clears
    // once the tab is actually closed, so a stale rejoin token doesn't
    // linger forever on a shared device.
    function saveRoleSession(role, token) {
        try { sessionStorage.setItem(ROLE_SESSION_KEY, JSON.stringify({ role, token })); } catch (e) {}
    }
    function getStoredRoleSession() {
        try { return JSON.parse(sessionStorage.getItem(ROLE_SESSION_KEY)); } catch (e) { return null; }
    }
    function clearRoleSession() {
        try { sessionStorage.removeItem(ROLE_SESSION_KEY); } catch (e) {}
    }
    let awaitingRejoin = false;

    const $statusDot = el('statusDot');
    const $connStatus = el('connectionStatus');
    const $ctrlStatus = el('controllerStatus');
    const $wifiClients = el('wifiClients');
    const $timerDisplay = el('timerDisplay');
    const $timerSub = el('timerSub');
    const $progressArc = el('progressArc');
    const $eventList = el('eventList');
    const $lapList = el('lapList');
    const $presetSelect = el('presetSelect');
    const $presetList = el('presetList');
    const $faultDisplay = el('faultDisplay');
    const $faultReason = el('faultReason');
    const $toast = el('toast');
    const $confirmOverlay = el('confirmOverlay');
    const $confirmMessage = el('confirmMessage');
    const $confirmOk = el('confirmOk');
    const $confirmCancel = el('confirmCancel');
    const $presetFormCard = el('presetFormCard');
    const $presetFormTitle = el('presetFormTitle');
    const $presetFormId = el('presetFormId');
    const $presetNameInput = el('presetNameInput');
    const $presetFreqInput = el('presetFreqInput');
    const $presetFwdAngleInput = el('presetFwdAngleInput');
    const $presetBackAngleInput = el('presetBackAngleInput');
    const $presetDurationInput = el('presetDurationInput');
    const $presetValidation = el('presetValidation');
    const $btnSavePreset = el('btnSavePreset');
    const $btnCancelPreset = el('btnCancelPreset');
    const $btnShowAddPreset = el('btnShowAddPreset');

    const $settingsLockedSection = el('settingsLockedSection');
    const $settingsUnlockedSection = el('settingsUnlockedSection');
    const $btnUnlockSettings = el('btnUnlockSettings');
    const $btnLockSettings = el('btnLockSettings');
    const $settingsPasswordInput = el('settingsPasswordInput');
    const $settingsLockPasswordInput = el('settingsLockPasswordInput');
    const $settingsAuthMessage = el('settingsAuthMessage');
    const $settingsLockMessage = el('settingsLockMessage');

    let selectedRole = null;
    let operatorAvailable = false;
    let judgeAvailable = false;

    // Theme functions
    function getStoredTheme() { return localStorage.getItem(THEME_STORAGE_KEY) || 'system'; }
    function setStoredTheme(theme) { localStorage.setItem(THEME_STORAGE_KEY, theme); }
    function getSystemTheme() { return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'; }
    function applyTheme(theme) {
        document.body.classList.remove('system', 'light', 'dark');
        document.body.classList.add(theme);
        const sunIcon = el('themeIconSun');
        const moonIcon = el('themeIconMoon');
        if (theme === 'light') {
            sunIcon.style.display = '';
            moonIcon.style.display = 'none';
        } else {
            sunIcon.style.display = 'none';
            moonIcon.style.display = '';
        }
    }
    let currentTheme = getStoredTheme() || getSystemTheme();
    applyTheme(currentTheme);

    el('themeToggle').addEventListener('click', () => {
        const newTheme = (currentTheme === 'light') ? 'dark' : 'light';
        currentTheme = newTheme;
        setStoredTheme(newTheme);
        applyTheme(newTheme);
    });

    // Fullscreen icon state
    function updateFullscreenIcon() {
        const isFullscreen = !!document.fullscreenElement;
        el('fsEnter').style.display = isFullscreen ? 'none' : '';
        el('fsExit').style.display = isFullscreen ? '' : 'none';
    }
    el('btnFullscreen').addEventListener('click', () => {
        if (!document.fullscreenElement) {
            document.documentElement.requestFullscreen().catch(()=>{});
        } else {
            document.exitFullscreen().catch(()=>{});
        }
    });
    document.addEventListener('fullscreenchange', updateFullscreenIcon);
    updateFullscreenIcon();

    function switchTab(tabId) {
        document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
        document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
        document.getElementById(`page-${tabId}`).classList.add('active');
        document.querySelector(`.tab-btn[data-tab="${tabId}"]`).classList.add('active');
    }

    document.querySelectorAll('.tab-btn').forEach(btn => {
        btn.addEventListener('click', () => switchTab(btn.dataset.tab));
    });

    function formatTime(ms) {
        const s = ms / 1000;
        const m = Math.floor(s / 60);
        const sec = Math.floor(s % 60);
        const mill = Math.floor((s % 1) * 1000);
        return `${String(m).padStart(2,'0')}:${String(sec).padStart(2,'0')}.${String(mill).padStart(3,'0')}`;
    }

    function saveSession() {
        try {
            localStorage.setItem(STORAGE_KEY, JSON.stringify({ lapCount, eventLog: state.eventLog || [] }));
        } catch(e) {}
    }

    function loadSession() {
        try { return JSON.parse(localStorage.getItem(STORAGE_KEY)); } catch(e) { return null; }
    }

    function mergeEvents(serverLog = []) {
        let merged = serverLog.slice();
        localEvents.forEach(localEvt => {
            let alreadyExists = merged.some(evt => evt.type === localEvt.type && evt.lapNumber === localEvt.lapNumber);
            if (!alreadyExists) merged.push(localEvt);
        });
        merged.sort((a, b) => a.elapsedMs - b.elapsedMs);
        localEvents = localEvents.filter(localEvt => {
            return !serverLog.some(evt => evt.type === localEvt.type && evt.lapNumber === localEvt.lapNumber);
        });
        return merged;
    }

    function addEvent(type, lapNumber) {
        const elapsedMs = timerRunning ? (timerElapsed + (performance.now() - timerOrigin)) : timerElapsed;
        const entry = {
            type,
            elapsedMs,
            lapNumber,
            role: currentRole || 'operator',
            localId: ++localEventCounter
        };
        localEvents.push(entry);
        renderEvents(mergeEvents(state.eventLog || []));
        saveSession();
    }

    function eventRowHtml(e) {
        let cls = '', label = '';
        if (e.type === 'LAP') {
            cls = 'event-lap';
            label = `LAP #${e.lapNumber} (${e.role || 'operator'})`;
        } else if (e.type === 'TEST_STARTED') {
            cls = 'event-start';
            label = 'TEST_STARTED';
        } else if (e.type === 'STOPPED' || e.type === 'TEST_ABORTED') {
            cls = 'event-stop';
            label = e.type;
        } else if (e.type === 'MOTOR_FAULT') {
            cls = 'event-fault';
            label = 'MOTOR_FAULT';
        } else if (e.type === 'PAUSED') {
            cls = 'event-pause';
            label = 'PAUSED';
        } else if (e.type === 'RESUMED') {
            cls = 'event-resume';
            label = 'RESUMED';
        } else if (e.type === 'CONNECTION_LOST') {
            cls = 'event-connection-lost';
            label = 'CONNECTION_LOST';
        } else if (e.type === 'CONNECTION_RESTORED') {
            cls = 'event-connection-restored';
            label = 'CONNECTION_RESTORED';
        } else {
            label = e.type;
        }
        return `<div class="${cls}"><span>${label}</span><span>${formatTime(e.elapsedMs)}</span></div>`;
    }

    function renderEvents(events) {
        if (!events) return;
        // Laps get their own dedicated log; everything else goes in the
        // general event log. Both show newest-first (most recent action
        // at the top), which is the opposite of the underlying array
        // (mergeEvents keeps it chronological ascending), so sort each
        // view descending independently rather than relying on the
        // incoming order.
        const laps = events.filter(e => e.type === 'LAP').sort((a, b) => b.elapsedMs - a.elapsedMs);
        const others = events.filter(e => e.type !== 'LAP').sort((a, b) => b.elapsedMs - a.elapsedMs);
        $lapList.innerHTML = laps.map(eventRowHtml).join('');
        $eventList.innerHTML = others.map(eventRowHtml).join('');
    }

    function startTimer() {
        if (timerRunning) return;
        timerRunning = true;
        timerOrigin = performance.now();
        requestAnimationFrame(updateTimer);
    }

    function pauseTimer() {
        if (!timerRunning) return;
        timerRunning = false;
        timerElapsed += performance.now() - timerOrigin;
        saveSession();
    }

    function resumeTimer() {
        if (timerRunning) return;
        timerRunning = true;
        timerOrigin = performance.now();
        requestAnimationFrame(updateTimer);
        saveSession();
    }

    function stopTimer() {
        timerRunning = false;
        timerElapsed = 0;
        updateTimer();
        saveSession();
    }

    function updateTimer() {
        let total = timerElapsed;
        if (timerRunning) total += performance.now() - timerOrigin;
        const seconds = total / 1000;
        const mins = Math.floor(seconds / 60);
        const secs = Math.floor(seconds % 60);
        const ms = Math.floor((seconds % 1) * 1000);
        $timerDisplay.textContent = `${String(mins).padStart(2,'0')}:${String(secs).padStart(2,'0')}.${String(ms).padStart(3,'0')}`;
        if (state.totalDurationSec && state.totalDurationSec > 0) {
            const remain = Math.max(0, state.totalDurationSec - seconds);
            const rMins = Math.floor(remain / 60);
            const rSecs = Math.floor(remain % 60);
            $timerSub.textContent = `Remaining: ${String(rMins).padStart(2,'0')}:${String(rSecs).padStart(2,'0')}.${String(Math.floor((remain%1)*1000)).padStart(3,'0')}`;
            const progress = Math.min(1, seconds / state.totalDurationSec);
            const circumference = 339.292;
            $progressArc.style.strokeDashoffset = circumference * (1 - progress);
        } else {
            $timerSub.textContent = '';
            $progressArc.style.strokeDashoffset = 339.292;
        }
        if (timerRunning) requestAnimationFrame(updateTimer);
    }

    function setStatusDot(stateStr) {
        $statusDot.className = 'status-dot';
        if (stateStr === 'RUNNING') $statusDot.classList.add('running');
        else if (stateStr === 'PAUSED') $statusDot.classList.add('paused');
        else if (stateStr === 'READY' || stateStr === 'STOPPED') $statusDot.classList.add('online');
    }

    let toastTimeout = null;
    function showToast(msg, duration = 2500) {
        $toast.textContent = msg;
        $toast.style.display = 'block';
        clearTimeout(toastTimeout);
        toastTimeout = setTimeout(() => { $toast.style.display = 'none'; }, duration);
    }

    function showConfirm(msg, callback) {
        $confirmMessage.textContent = msg;
        $confirmOverlay.classList.add('active');
        confirmCallback = callback;
    }
    $confirmCancel.addEventListener('click', () => { $confirmOverlay.classList.remove('active'); confirmCallback = null; });
    $confirmOk.addEventListener('click', () => { $confirmOverlay.classList.remove('active'); if (confirmCallback) confirmCallback(); confirmCallback = null; });

    // Cached authoritative limits
    let cachedLimits = { minFreq: 0.1, maxFreq: 10, minAngle: 1, maxAngle: 360 };
    function loadLimits() {
        fetch('/api/settings/public-limits').then(r => r.json()).then(data => {
            cachedLimits.minFreq = data.minFrequencyHz || 0.1;
            cachedLimits.maxFreq = data.maxFrequencyHz || 10;
            cachedLimits.minAngle = data.minStrokeAngleDeg || 1;
            cachedLimits.maxAngle = data.maxStrokeAngleDeg || 360;
        }).catch(()=>{});
    }

    function renderPresets(presets) {
        if (!presets) return;
        $presetSelect.innerHTML = '';
        presets.forEach(p => {
            const opt = document.createElement('option');
            opt.value = p.id;
            opt.textContent = p.name || `Preset ${p.id}`;
            $presetSelect.appendChild(opt);
        });
        $presetList.innerHTML = presets.map(p => `
            <div class="preset-item">
                <div class="preset-info">
                    <div class="preset-name">${p.name}</div>
                    <div class="preset-detail">${p.frequencyHz} Hz · ${p.forwardAngleDeg}°/${p.backwardAngleDeg}° · ${p.durationSeconds}s</div>
                </div>
                <div class="preset-actions">
                    <button class="btn secondary" data-edit="${p.id}">Edit</button>
                    <button class="btn danger" data-delete="${p.id}">Delete</button>
                </div>
            </div>
        `).join('');
        document.querySelectorAll('[data-edit]').forEach(btn => {
            btn.addEventListener('click', () => {
                const id = parseInt(btn.dataset.edit);
                const preset = presets.find(p => p.id === id);
                if (preset) showEditPreset(preset);
            });
        });
        document.querySelectorAll('[data-delete]').forEach(btn => {
            btn.addEventListener('click', () => {
                const id = parseInt(btn.dataset.delete);
                showConfirm('Delete this preset?', () => {
                    fetch(`/api/presets/${id}`, { method: 'DELETE' }).then(() => { showToast('Preset deleted'); refreshPresets(); }).catch(() => showToast('Failed to delete preset'));
                });
            });
        });
    }

    function refreshPresets() {
        fetch('/api/presets').then(r => r.json()).then(renderPresets).catch(() => showToast('Failed to load presets'));
    }

    function showEditPreset(preset) {
        editingPresetId = preset.id;
        $presetFormTitle.textContent = 'Edit Preset';
        $presetFormId.value = preset.id;
        $presetNameInput.value = preset.name;
        $presetFreqInput.value = preset.frequencyHz;
        $presetFwdAngleInput.value = preset.forwardAngleDeg;
        $presetBackAngleInput.value = preset.backwardAngleDeg;
        $presetDurationInput.value = preset.durationSeconds;
        $presetValidation.textContent = '';
        $presetFormCard.style.display = 'block';
        $btnShowAddPreset.textContent = 'Cancel';
    }

    function resetPresetForm() {
        editingPresetId = null;
        $presetFormTitle.textContent = 'New Preset';
        $presetFormId.value = '';
        $presetNameInput.value = '';
        $presetFreqInput.value = '';
        $presetFwdAngleInput.value = '';
        $presetBackAngleInput.value = '';
        $presetDurationInput.value = '';
        $presetValidation.textContent = '';
        $presetFormCard.style.display = 'none';
        $btnShowAddPreset.textContent = '+ New Preset';
    }

    $btnShowAddPreset.addEventListener('click', () => {
        if ($presetFormCard.style.display === 'block') resetPresetForm();
        else {
            editingPresetId = null;
            $presetFormTitle.textContent = 'New Preset';
            $presetFormId.value = '';
            $presetNameInput.value = '';
            $presetFreqInput.value = '';
            $presetFwdAngleInput.value = '';
            $presetBackAngleInput.value = '';
            $presetDurationInput.value = '';
            $presetValidation.textContent = '';
            $presetFormCard.style.display = 'block';
            $btnShowAddPreset.textContent = 'Cancel';
        }
    });

    $btnCancelPreset.addEventListener('click', resetPresetForm);

    $btnSavePreset.addEventListener('click', () => {
        const id = $presetFormId.value ? parseInt($presetFormId.value) : null;
        const name = $presetNameInput.value.trim() || 'Unnamed';
        const freq = parseFloat($presetFreqInput.value);
        const fwd = parseFloat($presetFwdAngleInput.value);
        const back = parseFloat($presetBackAngleInput.value);
        const dur = parseInt($presetDurationInput.value);
        if (isNaN(freq) || isNaN(fwd) || isNaN(back) || isNaN(dur)) {
            $presetValidation.textContent = 'Please fill all numeric fields.';
            return;
        }
        if (freq < cachedLimits.minFreq || freq > cachedLimits.maxFreq) {
            $presetValidation.textContent = `Frequency must be between ${cachedLimits.minFreq} and ${cachedLimits.maxFreq} Hz.`;
            return;
        }
        if (fwd < cachedLimits.minAngle || fwd > cachedLimits.maxAngle) {
            $presetValidation.textContent = `Forward angle must be between ${cachedLimits.minAngle} and ${cachedLimits.maxAngle}°.`;
            return;
        }
        if (back < cachedLimits.minAngle || back > cachedLimits.maxAngle) {
            $presetValidation.textContent = `Backward angle must be between ${cachedLimits.minAngle} and ${cachedLimits.maxAngle}°.`;
            return;
        }
        const payload = { name, frequencyHz: freq, forwardAngleDeg: fwd, backwardAngleDeg: back, durationSeconds: dur };
        const url = id ? `/api/presets/${id}` : '/api/presets';
        const method = id ? 'PUT' : 'POST';
        fetch(url, { method, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) })
            .then(r => r.json().then(data => ({ status: r.status, data })))
            .then(({ status, data }) => {
                if (status >= 400) {
                    $presetValidation.textContent = data.error || 'Validation error';
                    showToast(data.error || 'Failed to save preset');
                    return;
                }
                showToast(id ? 'Preset updated' : 'Preset added');
                resetPresetForm();
                refreshPresets();
            })
            .catch(() => { $presetValidation.textContent = 'Network error'; showToast('Failed to save preset'); });
    });

    function setSettingsUnlocked(unlocked) {
        settingsUnlocked = unlocked;
        $settingsLockedSection.style.display = unlocked ? 'none' : 'block';
        $settingsUnlockedSection.style.display = unlocked ? 'block' : 'none';
    }

    $btnUnlockSettings.addEventListener('click', () => {
        const password = $settingsPasswordInput.value;
        if (!password) { $settingsAuthMessage.textContent = 'Please enter password'; return; }
        fetch('/api/settings/unlock', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ password }) })
            .then(r => r.json().then(data => ({ status: r.status, data })))
            .then(({ status, data }) => {
                if (status === 200) {
                    $settingsAuthMessage.textContent = '';
                    $settingsPasswordInput.value = '';
                    setSettingsUnlocked(true);
                    showToast('Settings unlocked');
                    loadProtectedSettings();
                    loadLimits();
                } else {
                    $settingsAuthMessage.textContent = data.error || 'Wrong password';
                }
            })
            .catch(() => { $settingsAuthMessage.textContent = 'Network error'; });
    });

    $btnLockSettings.addEventListener('click', () => {
        const password = $settingsLockPasswordInput.value;
        if (!password) { $settingsLockMessage.textContent = 'Password required to lock'; return; }
        fetch('/api/settings/lock', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ password }) })
            .then(r => r.json().then(data => ({ status: r.status, data })))
            .then(({ status, data }) => {
                if (status === 200) {
                    setSettingsUnlocked(false);
                    $settingsLockPasswordInput.value = '';
                    $settingsLockMessage.textContent = '';
                    showToast('Settings locked');
                } else {
                    $settingsLockMessage.textContent = data.error || 'Wrong password';
                }
            })
            .catch(() => { $settingsLockMessage.textContent = 'Network error'; });
    });

    // Wraps fetch() for any /api/settings* request that requires the
    // Settings panel to be unlocked. The unlock now auto-expires on the
    // device after inactivity (see Config::SETTINGS_UNLOCK_TIMEOUT_MS), so
    // a 401 here is a normal, expected outcome - not just an error case -
    // and needs to actually re-lock the UI instead of just showing a
    // generic failure toast.
    function fetchAuthed(url, opts) {
        return fetch(url, opts).then(r => {
            if (r.status === 401 && settingsUnlocked) {
                setSettingsUnlocked(false);
                showToast('Settings session expired - please unlock again');
            }
            return r.json().then(data => ({ status: r.status, data }));
        });
    }

    function loadProtectedSettings() {
        fetchAuthed('/api/settings').then(({ status, data }) => {
            if (status !== 200) return;
            el('settingsSsid').value = data.ssid || '';
            el('settingsLed').checked = data.ledEnabled !== false;
            el('settingsMinFreq').value = data.minFrequencyHz || 0.1;
            el('settingsMaxFreq').value = data.maxFrequencyHz || 10;
            el('settingsMinAngle').value = data.minStrokeAngleDeg || 1;
            el('settingsMaxAngle').value = data.maxStrokeAngleDeg || 360;
            el('settingsMotorEnableX').checked = data.motorEnableX || false;
            el('settingsMotorEnableY').checked = data.motorEnableY || false;
        }).catch(() => showToast('Failed to load settings'));
        fetchAuthed('/api/settings/timeout').then(({ status, data }) => {
            if (status !== 200) return;
            el('settingsTimeout').value = data.controllerTimeoutMs || 2000;
        }).catch(() => showToast('Failed to load timeout'));
        fetchAuthed('/api/settings/passwords').then(({ status, data }) => {
            if (status !== 200) return;
            const statusParts = [];
            if (data.operatorSet) statusParts.push('Operator password changed');
            if (data.judgeSet) statusParts.push('Judge password changed');
            if (data.settingsSet) statusParts.push('Settings password changed');
            el('passwordStatus').textContent = statusParts.length ? statusParts.join('; ') : 'All default';
        }).catch(() => showToast('Failed to load password status'));
    }

    el('btnSaveWifi').addEventListener('click', () => {
        const ssid = el('settingsSsid').value.trim();
        const pass = el('settingsPassword').value;
        if (!ssid) { showToast('SSID required'); return; }
        if (pass && pass.length < 8) { showToast('Password must be at least 8 characters'); return; }
        fetchAuthed('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ ssid, password: pass }) })
            .then(({ status }) => {
                if (status !== 200) return;
                showToast('Wi-Fi saved, restarting...');
                setTimeout(() => fetch('/api/restart', { method: 'POST' }).catch(() => {}), 500);
            })
            .catch(() => showToast('Failed to save Wi-Fi settings'));
    });

    el('btnSaveLimits').addEventListener('click', () => {
        const minFreq = parseFloat(el('settingsMinFreq').value);
        const maxFreq = parseFloat(el('settingsMaxFreq').value);
        const minAngle = parseFloat(el('settingsMinAngle').value);
        const maxAngle = parseFloat(el('settingsMaxAngle').value);
        if (isNaN(minFreq) || isNaN(maxFreq) || minFreq >= maxFreq) { showToast('Invalid frequency limits'); return; }
        if (isNaN(minAngle) || isNaN(maxAngle) || minAngle >= maxAngle) { showToast('Invalid angle limits'); return; }
        fetchAuthed('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ minFrequencyHz: minFreq, maxFrequencyHz: maxFreq, minStrokeAngleDeg: minAngle, maxStrokeAngleDeg: maxAngle }) })
            .then(({ status }) => {
                if (status !== 200) return;
                showToast('Motion limits saved, restarting...');
                setTimeout(() => fetch('/api/restart', { method: 'POST' }).catch(() => {}), 500);
            })
            .catch(() => showToast('Failed to save motion limits'));
    });

    el('btnSaveTimeout').addEventListener('click', () => {
        const timeout = parseInt(el('settingsTimeout').value);
        if (isNaN(timeout) || timeout < 1000 || timeout > 5000) { showToast('Timeout must be 1000-5000ms'); return; }
        fetchAuthed('/api/settings/timeout', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ timeoutMs: timeout }) })
            .then(({ status }) => { if (status === 200) showToast('Timeout updated'); })
            .catch(() => showToast('Failed to update timeout'));
    });

    el('btnSaveLed').addEventListener('click', () => {
        const enabled = el('settingsLed').checked;
        fetchAuthed('/api/settings/led', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ enabled }) })
            .then(({ status }) => { if (status === 200) showToast('LED setting updated'); })
            .catch(() => showToast('Failed'));
    });

    el('btnSaveMotorEnable').addEventListener('click', () => {
        const x = el('settingsMotorEnableX').checked;
        const y = el('settingsMotorEnableY').checked;
        fetchAuthed('/api/settings/motor-enable', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ x, y }) })
            .then(({ status }) => { if (status === 200) showToast('Motor enable setting updated'); })
            .catch(() => showToast('Failed'));
    });

    el('btnSavePasswords').addEventListener('click', () => {
        const operator = el('settingsOpPass').value;
        const judge = el('settingsJudgePass').value;
        const settings = el('settingsSetPass').value;
        if (operator.length < 8 || judge.length < 8 || settings.length < 8) {
            showToast('Passwords must be at least 8 characters');
            return;
        }
        fetchAuthed('/api/settings/passwords', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ operator, judge, settings }) })
            .then(({ status }) => {
                if (status !== 200) return;
                showToast('Passwords updated');
                loadProtectedSettings();
            })
            .catch(() => showToast('Failed to update passwords'));
    });

    el('btnRestoreDefaults').addEventListener('click', () => {
        showConfirm('Restore factory defaults?', () => {
            fetchAuthed('/api/settings/restore-defaults', { method: 'POST' })
                .then(({ status }) => {
                    if (status !== 200) return;
                    showToast('Defaults restored, restarting...');
                    setTimeout(() => fetch('/api/restart', { method: 'POST' }).catch(() => {}), 500);
                })
                .catch(() => showToast('Failed'));
        });
    });

    el('btnUploadFirmware').addEventListener('click', () => {
        const file = el('firmwareFile').files[0];
        if (!file) { showToast('Choose a .bin file first'); return; }
        if (lastState === 'RUNNING' || lastState === 'PAUSED' || lastState === 'COUNTDOWN') {
            showToast('Stop the current run before updating firmware');
            return;
        }
        showConfirm(`Upload ${file.name} and update firmware? The device will restart when done.`, () => {
            const progress = el('firmwareProgress');
            const status = el('firmwareStatus');
            const btn = el('btnUploadFirmware');
            btn.disabled = true;
            progress.style.display = 'block';
            progress.value = 0;
            status.textContent = 'Uploading...';

            const formData = new FormData();
            formData.append('firmware', file);

            // XHR instead of fetch() here specifically for real upload
            // progress - fetch() doesn't expose it cleanly, and a
            // multi-second firmware upload with no feedback at all is a
            // bad experience.
            const xhr = new XMLHttpRequest();
            xhr.open('POST', '/api/ota/upload');
            xhr.upload.addEventListener('progress', (e) => {
                if (e.lengthComputable) progress.value = Math.round((e.loaded / e.total) * 100);
            });
            xhr.onload = () => {
                btn.disabled = false;
                let data = {};
                try { data = JSON.parse(xhr.responseText); } catch (e) {}
                if (xhr.status === 200 && data.success) {
                    status.textContent = 'Update successful - restarting...';
                    showToast('Firmware updated, restarting...');
                    setTimeout(() => fetch('/api/restart', { method: 'POST' }).catch(() => {}), 500);
                } else if (xhr.status === 401) {
                    status.textContent = data.error || 'Unauthorized, or a run is active';
                    if (settingsUnlocked) {
                        setSettingsUnlocked(false);
                        showToast('Settings session expired - please unlock again');
                    } else {
                        showToast(status.textContent);
                    }
                } else {
                    status.textContent = data.error || 'Update failed';
                    showToast('Firmware update failed');
                }
            };
            xhr.onerror = () => {
                btn.disabled = false;
                status.textContent = 'Upload failed - connection lost';
                showToast('Upload failed');
            };
            xhr.send(formData);
        });
    });

    function updateRoleButtons() {
        if (operatorAvailable) {
            el('btnSelectOperator').disabled = false;
            el('btnSelectOperator').style.opacity = '1';
        } else {
            el('btnSelectOperator').disabled = true;
            el('btnSelectOperator').style.opacity = '0.4';
        }
        if (judgeAvailable) {
            el('btnSelectJudge').disabled = false;
            el('btnSelectJudge').style.opacity = '1';
        } else {
            el('btnSelectJudge').disabled = true;
            el('btnSelectJudge').style.opacity = '0.4';
        }
    }

    function resetRoleOverlay() {
        el('roleError').textContent = '';
        el('rolePasswordSection').style.display = 'none';
        el('btnSelectOperator').classList.remove('selected');
        el('btnSelectJudge').classList.remove('selected');
    }

    function showPasswordInput(role) {
        selectedRole = role;
        el('roleError').textContent = '';
        el('rolePasswordSection').style.display = 'block';
        el('rolePasswordInput').value = '';
        el('rolePasswordInput').focus();

        const opBtn = el('btnSelectOperator');
        const judgeBtn = el('btnSelectJudge');
        if (role === 'operator') {
            opBtn.classList.add('selected');
            judgeBtn.classList.remove('selected');
        } else {
            judgeBtn.classList.add('selected');
            opBtn.classList.remove('selected');
        }
    }

    el('btnSelectOperator').addEventListener('click', () => showPasswordInput('operator'));
    el('btnSelectJudge').addEventListener('click', () => showPasswordInput('judge'));

    el('btnSwitchRole').addEventListener('click', () => {
        if (!currentRole) return; // nothing to switch away from
        showConfirm('Switch role? You will need to select and authenticate again.', () => {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({ cmd: 'releaseRole' }));
            } else {
                // Not connected right now - just clear locally so the
                // picker shows once we do reconnect.
                currentRole = null;
                clearRoleSession();
                resetRoleOverlay();
                el('roleOverlay').classList.add('active');
            }
        });
    });

    // Warn before an accidental refresh/close while a run is actually in
    // progress. Browsers show their own generic confirmation text here -
    // they don't allow customizing it (a security measure so sites can't
    // show a misleading message) - but the prompt itself still does the
    // job of catching a stray reload.
    window.addEventListener('beforeunload', (e) => {
        if (lastState === 'RUNNING' || lastState === 'PAUSED' || lastState === 'COUNTDOWN') {
            e.preventDefault();
            e.returnValue = '';
        }
    });

    el('btnRoleSubmit').addEventListener('click', () => {
        const password = el('rolePasswordInput').value;
        if (!password) { el('roleError').textContent = 'Enter password'; return; }
        ws.send(JSON.stringify({ cmd: 'selectRole', role: selectedRole, password }));
    });

    function connect() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const url = `${protocol}//${window.location.host}/ws`;
        ws = new WebSocket(url);
        ws.onopen = () => {
            $connStatus.textContent = 'Connected';
            $connStatus.style.color = 'var(--text-secondary)';
            reconnectAttempt = 0;
            // Motion limits may have changed server-side since our last
            // connection (e.g. an operator updated them, which triggers a
            // device restart). Refresh the cached copy used for client-side
            // preset validation so it can't go stale across a reconnect.
            loadLimits();

            const stored = getStoredRoleSession();
            if (stored && stored.role && stored.token) {
                // Try to silently reclaim our previous role (e.g. this is
                // a page refresh) instead of forcing the picker on every
                // reconnect. If it fails (grace window expired, or someone
                // else already took the role), the 'error' handler below
                // falls back to showing the picker.
                awaitingRejoin = true;
                ws.send(JSON.stringify({ cmd: 'rejoinRole', role: stored.role, token: stored.token }));
            } else {
                el('roleOverlay').classList.add('active');
            }
        };
        ws.onmessage = (ev) => {
            try {
                const msg = JSON.parse(ev.data);
                if (msg.type === 'roleStatus') {
                    operatorAvailable = msg.operatorAvailable;
                    judgeAvailable = msg.judgeAvailable;
                    updateRoleButtons();
                } else if (msg.type === 'roleAssigned') {
                    currentRole = msg.role;
                    awaitingRejoin = false;
                    if (msg.token) saveRoleSession(msg.role, msg.token);
                    el('roleOverlay').classList.remove('active');
                    configureUI();
                    if (currentRole === 'operator') {
                        clearInterval(window._pingInterval);
                        window._pingInterval = setInterval(() => {
                            if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ cmd: 'ping' }));
                        }, 500);
                    }
                } else if (msg.type === 'roleReleased') {
                    currentRole = null;
                    clearInterval(window._pingInterval);
                    clearRoleSession();
                    configureUI();
                    resetRoleOverlay();
                    el('roleOverlay').classList.add('active');
                } else if (msg.type === 'status') {
                    updateState(msg);
                } else if (msg.type === 'motionStarted') {
                    timerElapsed = 0;
                    startTimer();
                    addEvent('TEST_STARTED');
                    showToast('Experiment started');
                } else if (msg.type === 'commandAck') {
                    handleAck(msg);
                } else if (msg.type === 'error') {
                    if (awaitingRejoin) {
                        // Our silent rejoin attempt was rejected (grace
                        // window expired, or another device already
                        // claimed the role). Fall back to the normal
                        // picker instead of leaving the user stuck with
                        // no UI and no explanation.
                        awaitingRejoin = false;
                        clearRoleSession();
                        resetRoleOverlay();
                        el('roleOverlay').classList.add('active');
                    }
                    const roleError = el('roleError');
                    if (roleError && el('roleOverlay').classList.contains('active')) {
                        roleError.textContent = msg.message || 'Error';
                    } else {
                        showToast(msg.message || 'Error');
                    }
                }
            } catch(e) {}
        };
        ws.onclose = () => {
            $connStatus.textContent = 'Disconnected';
            $connStatus.style.color = 'var(--red)';
            pendingCommands = {};
            if (reconnectAttempt < MAX_RECONNECT) {
                reconnectAttempt++;
                const delay = Math.min(2000 * reconnectAttempt, 10000);
                setTimeout(connect, delay);
            } else showToast('Could not reconnect to controller', 5000);
        };
        ws.onerror = () => { ws.close(); };
    }

    function configureUI() {
        if (currentRole === 'operator') {
            el('tabPresets').style.display = 'block';
            el('tabSettings').style.display = 'block';
            el('operatorControls').style.display = 'block';
            el('btnSwitchRole').style.display = 'inline-flex';
        } else if (currentRole === 'judge') {
            el('tabPresets').style.display = 'none';
            el('tabSettings').style.display = 'none';
            el('operatorControls').style.display = 'none';
            $ctrlStatus.textContent = 'Judge';
            el('btnSwitchRole').style.display = 'inline-flex';
        } else {
            el('tabPresets').style.display = 'none';
            el('tabSettings').style.display = 'none';
            el('operatorControls').style.display = 'none';
            el('btnSwitchRole').style.display = 'none';
        }
    }

    function updateState(data) {
        const prevState = lastState;
        lastState = data.state;
        state = { ...data, eventLog: mergeEvents(data.eventLog || []) };
        setStatusDot(data.state);
        $ctrlStatus.textContent = data.isOperator ? 'Operator' : (data.isJudge ? 'Judge' : 'Observer');
        $wifiClients.textContent = `${data.wifiClients || 0} clients`;

        if (prevState !== data.state) {
            if (data.state === 'RUNNING' && prevState !== 'RUNNING') {
                if (!timerRunning) {
                    timerElapsed = data.elapsedMs || 0;
                    startTimer();
                }
            } else if (data.state === 'PAUSED' && prevState !== 'PAUSED') {
                if (timerRunning) pauseTimer();
            } else if (data.state === 'STOPPED' && prevState !== 'STOPPED') {
                stopTimer();
            } else if (data.state === 'READY' && prevState === 'STOPPED') {
                stopTimer();
            }
        }

        const canStart = data.state === 'READY' || data.state === 'STOPPED';
        el('btnStart').disabled = !canStart;
        el('btnPause').disabled = data.state !== 'RUNNING';
        el('btnResume').disabled = data.state !== 'PAUSED';
        el('btnStop').disabled = !['RUNNING','PAUSED','COUNTDOWN'].includes(data.state);
        el('btnLap').disabled = !['RUNNING','PAUSED'].includes(data.state);

        if (data.lastFaultReason && data.lastFaultReason.length) {
            $faultDisplay.style.display = 'block';
            $faultReason.textContent = data.lastFaultReason;
        } else $faultDisplay.style.display = 'none';

        renderEvents(state.eventLog);
        updateTimer();
    }

    function handleAck(ack) {
        if (!ack.success) { showToast(ack.message || 'Command failed'); return; }
        const cmdType = pendingCommands[ack.requestId];
        if (cmdType) {
            delete pendingCommands[ack.requestId];
            switch (cmdType) {
                case 'pause': pauseTimer(); addEvent('PAUSED'); break;
                case 'resume': resumeTimer(); addEvent('RESUMED'); break;
                case 'stop': stopTimer(); addEvent('STOPPED'); break;
                default: break;
            }
            saveSession();
        }
    }

    function sendCommand(cmd, payload = {}) {
        if (!ws || ws.readyState !== WebSocket.OPEN) { showToast('Not connected'); return; }
        const requestId = Date.now().toString(36) + Math.random().toString(36).substr(2, 4);
        pendingCommands[requestId] = cmd;
        ws.send(JSON.stringify({ cmd, requestId, ...payload }));
    }

    el('btnStart').addEventListener('click', () => {
        const presetId = parseInt($presetSelect.value);
        if (!presetId) { showToast('Select a preset'); return; }
        sendCommand('start', { presetId });
    });
    el('btnPause').addEventListener('click', () => sendCommand('pause'));
    el('btnResume').addEventListener('click', () => sendCommand('resume'));
    el('btnStop').addEventListener('click', () => showConfirm('Stop the running experiment?', () => sendCommand('stop')));
    el('btnLap').addEventListener('click', () => {
        if (!['RUNNING','PAUSED'].includes(state.state)) { showToast('Experiment not running'); return; }
        const lapTime = timerRunning ? (timerElapsed + (performance.now() - timerOrigin)) : timerElapsed;
        lapCount++;
        addEvent('LAP', lapCount);
        sendCommand('lap');
        showToast(`LAP #${lapCount} @ ${formatTime(lapTime)}`);
        if (navigator.vibrate) navigator.vibrate(30);
    });

    function isEditableTarget(target) { return target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.tagName === 'SELECT' || target.isContentEditable; }
    document.addEventListener('keydown', (e) => {
        if (isEditableTarget(e.target)) return;
        if (currentRole !== 'operator') {
            if (e.key === ' ' && !el('btnLap').disabled) {
                e.preventDefault();
                el('btnLap').click();
            }
            return;
        }
        switch (e.key.toLowerCase()) {
            case 's':
            case 'n': if (!el('btnStart').disabled) el('btnStart').click(); break;
            case 'x': if (!el('btnStop').disabled) el('btnStop').click(); break;
            case 'p': if (!el('btnPause').disabled) el('btnPause').click(); break;
            case 'r': if (!el('btnResume').disabled) el('btnResume').click(); break;
            case ' ': e.preventDefault(); if (!el('btnLap').disabled) el('btnLap').click(); break;
        }
    });

    const saved = loadSession();
    if (saved) { lapCount = saved.lapCount || 0; state.eventLog = saved.eventLog || []; renderEvents(state.eventLog); }
    refreshPresets();
    loadLimits();
    setSettingsUnlocked(false);
    $settingsAuthMessage.textContent = '';
    $settingsLockMessage.textContent = '';
    connect();
    switchTab('home');
})();
