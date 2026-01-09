#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "config.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "lcd_driver.h"
#include "settings.h"
#include "tfnsw_client.h"
#include "rgb_led.h"

static const char *TAG = "web_server";

static httpd_handle_t server = NULL;
static display_cmd_cb_t display_callback = NULL;
static system_cmd_cb_t system_callback = NULL;
static api_key_set_cb_t api_key_callback = NULL;

// ============================================================================
// HTML Content
// ============================================================================

static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 LCD Control</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: #1a1a2e;
            color: #eee;
            min-height: 100vh;
            padding: 20px;
        }
        .container { max-width: 600px; margin: 0 auto; }
        h1 { color: #ffe000; margin-bottom: 20px; text-align: center; }
        .card {
            background: #16213e;
            border-radius: 12px;
            padding: 20px;
            margin-bottom: 20px;
        }
        .card h2 { color: #ffe000; margin-bottom: 15px; font-size: 1.2em; }
        .status-row {
            display: flex;
            justify-content: space-between;
            padding: 8px 0;
            border-bottom: 1px solid #2a3f5f;
        }
        .status-row:last-child { border-bottom: none; }
        .status-label { color: #888; }
        .status-value { color: #fff; font-weight: 500; }
        .badge { display: inline-block; padding: 2px 8px; border-radius: 4px; font-size: 11px; margin-left: 8px; }
        .badge-green { background: #4caf50; color: #fff; }
        .badge-red { background: #f44336; color: #fff; }
        .badge-yellow { background: #ffe000; color: #000; }
        .btn {
            display: inline-block;
            padding: 12px 24px;
            border: none;
            border-radius: 8px;
            cursor: pointer;
            font-size: 14px;
            font-weight: 500;
            margin: 5px;
        }
        .btn:hover { opacity: 0.85; transform: scale(0.98); }
        .btn-primary { background: #ffe000; color: #000; }
        .btn-danger { background: #f44336; color: #fff; }
        .btn-success { background: #4caf50; color: #fff; }
        .btn-secondary { background: #2a3f5f; color: #fff; }
        .btn-scene { background: #2a3f5f; color: #fff; flex: 1; }
        .btn-scene.active { background: #ffe000; color: #000; }
        .btn-group { display: flex; flex-wrap: wrap; gap: 10px; margin-top: 15px; }
        input[type="range"] { width: 100%; margin: 10px 0; accent-color: #ffe000; }
        .slider-label { display: flex; justify-content: space-between; }
        .form-group { margin-bottom: 15px; }
        input[type="text"], input[type="password"] {
            width: 100%;
            padding: 10px;
            border: 1px solid #2a3f5f;
            border-radius: 6px;
            background: #1a1a2e;
            color: #fff;
            margin-top: 5px;
        }
        input[type="text"]:focus, input[type="password"]:focus {
            border-color: #ffe000;
            outline: none;
        }
        label { color: #888; }
        .color-grid { display: grid; grid-template-columns: repeat(7, 1fr); gap: 8px; margin-top: 10px; }
        .color-btn {
            width: 100%;
            aspect-ratio: 1;
            border: 2px solid transparent;
            border-radius: 8px;
            cursor: pointer;
            transition: transform 0.1s, border-color 0.1s;
        }
        .color-btn:hover { transform: scale(1.1); }
        .color-btn.active { border-color: #fff; }
        .scene-info { font-size: 12px; color: #666; margin-top: 8px; text-align: center; }
        .view-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; }
        .view-btn {
            display: flex; flex-direction: column; align-items: center; justify-content: center;
            padding: 12px 8px; border: 2px solid #2a3f5f; border-radius: 10px;
            background: #1a1a2e; cursor: pointer; transition: all 0.2s;
        }
        .view-btn:hover { border-color: #ffe000; transform: scale(0.98); }
        .view-btn.active { border-color: #ffe000; background: #2a3f5f; }
        .view-btn .color-dot { width: 10px; height: 10px; border-radius: 50%; margin-bottom: 6px; }
        .view-btn .view-name { color: #fff; font-weight: 500; font-size: 14px; }
        .view-btn .view-source { font-size: 10px; color: #888; margin-top: 4px; }
        .view-btn .view-source.realtime { color: #4caf50; }
        .view-btn .view-source.static { color: #ff9800; }
        .progress-bar { background: #2a3f5f; border-radius: 4px; height: 8px; margin-top: 8px; overflow: hidden; }
        .progress-fill { background: #ffe000; height: 100%; transition: width 0.3s; }
        .sd-info { font-size: 12px; color: #888; margin-top: 4px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Departure Board</h1>
        <div class="card">
            <h2>System Status</h2>
            <div id="status">Loading...</div>
        </div>
        <div class="card">
            <h2>SD Card Storage</h2>
            <div id="sd-status">Checking...</div>
        </div>
        <div class="card">
            <h2>TfNSW Live Data</h2>
            <div id="tfnsw-status">Checking...</div>
            <form id="apikey-form" style="margin-top: 15px;">
                <div class="form-group">
                    <label>TfNSW API Key</label>
                    <input type="password" id="apikey" placeholder="Enter your API key">
                </div>
                <div class="btn-group">
                    <button type="submit" class="btn btn-primary">Save API Key</button>
                    <button type="button" class="btn btn-secondary" onclick="clearApiKey()">Clear</button>
                    <button type="button" class="btn btn-success" onclick="refreshDepartures()">Refresh</button>
                </div>
            </form>
            <div class="scene-info">Get your free API key from <a href="https://opendata.transport.nsw.gov.au" target="_blank" style="color:#ffe000">opendata.transport.nsw.gov.au</a></div>
        </div>
        <div class="card">
            <h2>Display View</h2>
            <div id="current-view-info" style="margin-bottom:12px;padding:10px;background:#1a1a2e;border-radius:8px;text-align:center;">
                <span style="color:#888">Current:</span> <span id="current-view-name" style="color:#ffe000;font-weight:600">Loading...</span>
            </div>
            <div class="view-grid" id="view-btns">Loading views...</div>
            <div class="scene-info">Press the button on device to cycle views</div>
        </div>
        <div class="card">
            <h2>Theme Color</h2>
            <div class="color-grid" id="color-grid">
                <button class="color-btn" style="background:#00e0ff" data-color="16769024" onclick="setTheme(16769024)" title="Teal"></button>
                <button class="color-btn" style="background:#0080ff" data-color="16744448" onclick="setTheme(16744448)" title="Blue"></button>
                <button class="color-btn" style="background:#4444ff" data-color="16729156" onclick="setTheme(16729156)" title="Purple"></button>
                <button class="color-btn" style="background:#ff00ff" data-color="16711935" onclick="setTheme(16711935)" title="Magenta"></button>
                <button class="color-btn" style="background:#ffd400" data-color="54527" onclick="setTheme(54527)" title="Yellow"></button>
                <button class="color-btn" style="background:#80ff00" data-color="65408" onclick="setTheme(65408)" title="Lime"></button>
                <button class="color-btn" style="background:#ffffff" data-color="16777215" onclick="setTheme(16777215)" title="White"></button>
            </div>
        </div>
        <div class="card">
            <h2>Status LED</h2>
            <div class="color-grid" id="led-grid">
                <button class="color-btn" style="background:#00ffff" data-led="65535" onclick="setLed(65535)" title="Teal"></button>
                <button class="color-btn" style="background:#ffff00" data-led="16776960" onclick="setLed(16776960)" title="Yellow"></button>
                <button class="color-btn" style="background:#00ff00" data-led="65280" onclick="setLed(65280)" title="Green"></button>
                <button class="color-btn" style="background:#ff8000" data-led="16744448" onclick="setLed(16744448)" title="Orange"></button>
                <button class="color-btn" style="background:#ff0000" data-led="16711680" onclick="setLed(16711680)" title="Red"></button>
                <button class="color-btn" style="background:#ff00ff" data-led="16711935" onclick="setLed(16711935)" title="Magenta"></button>
                <button class="color-btn" style="background:#0000ff" data-led="255" onclick="setLed(255)" title="Blue"></button>
            </div>
            <div class="btn-group" style="margin-top:10px">
                <button class="btn btn-secondary" onclick="setLedAuto()" id="led-auto-btn">Auto (Follow View)</button>
                <button class="btn btn-secondary" onclick="setLedOff()">Off</button>
            </div>
            <div class="scene-info">Auto mode: LED color follows current view's accent color</div>
        </div>
        <div class="card">
            <h2>Display Control</h2>
            <div class="slider-label">
                <span>Brightness</span>
                <span id="brightness-value">20</span>%
            </div>
            <input type="range" id="brightness" min="0" max="100" value="20"
                   oninput="document.getElementById('brightness-value').textContent=this.value"
                   onchange="setBrightness()">
            <div class="btn-group">
                <button class="btn btn-primary" onclick="sendCmd('clear')">Clear Display</button>
                <button class="btn btn-primary" onclick="sendCmd('splash')">Show Splash</button>
            </div>
        </div>
        <div class="card">
            <h2>WiFi Configuration</h2>
            <form id="wifi-form">
                <div class="form-group">
                    <label>SSID</label>
                    <input type="text" id="wifi-ssid" placeholder="Network name">
                </div>
                <div class="form-group">
                    <label>Password</label>
                    <input type="password" id="wifi-pass" placeholder="Password">
                </div>
                <button type="submit" class="btn btn-primary">Save & Connect</button>
            </form>
        </div>
        <div class="card">
            <h2>System</h2>
            <div class="btn-group">
                <button class="btn btn-secondary" onclick="settingsAction('clear_log')">Clear Log</button>
                <button class="btn btn-danger" onclick="settingsAction('reset')">Reset Settings</button>
                <button class="btn btn-danger" onclick="sysCmd('restart')">Restart</button>
                <button class="btn btn-danger" onclick="sysCmd('reset_wifi')">Reset WiFi</button>
            </div>
        </div>
        <div class="card">
            <h2>Debug Info</h2>
            <div id="debug-info">Loading...</div>
            <div class="btn-group" style="margin-top:10px">
                <button class="btn btn-secondary" onclick="fetchDebug()">Refresh Debug</button>
            </div>
        </div>
    </div>
    <script>
        let currentView = 0;
        let currentViewName = '';
        let currentTheme = 16769024;
        let viewsData = [];
        let viewsLoaded = false;
        function rgbToHex(rgb) {
            // Convert RGB888 to CSS hex (handles BGR swap)
            const r = (rgb >> 16) & 0xFF;
            const g = (rgb >> 8) & 0xFF;
            const b = rgb & 0xFF;
            return '#' + [b,g,r].map(x => x.toString(16).padStart(2,'0')).join('');
        }
        async function fetchViews() {
            try {
                const res = await fetch('/api/views');
                const data = await res.json();
                viewsData = data.views || [];
                currentView = data.current;
                currentViewName = data.current_name;
                const container = document.getElementById('view-btns');
                container.innerHTML = viewsData.filter(v => v.enabled).map(v => {
                    const colorHex = rgbToHex(v.accent_color);
                    const sourceLabel = v.data_source === 'realtime' ? '● Live' : '◆ Demo';
                    const sourceClass = v.data_source;
                    return `<div class="view-btn${v.active ? ' active' : ''}" data-view="${v.id}" onclick="setView(${v.id})">
                        <div class="color-dot" style="background:${colorHex}"></div>
                        <span class="view-name">${v.name}</span>
                        <span class="view-source ${sourceClass}">${sourceLabel}</span>
                    </div>`;
                }).join('');
                viewsLoaded = true;
                document.getElementById('current-view-name').textContent = currentViewName;
            } catch(e) {
                document.getElementById('view-btns').innerHTML = '<span style="color:#f44336">Error loading views</span>';
            }
        }
        async function fetchStatus() {
            try {
                const res = await fetch('/api/status');
                const d = await res.json();
                const newView = d.view !== undefined ? d.view : (d.scene || 0);
                if (newView !== currentView) {
                    currentView = newView;
                    updateViewButtons();
                }
                currentTheme = d.theme_color || 16769024;
                updateThemeButtons();
                if (d.settings && d.settings.brightness) {
                    document.getElementById('brightness').value = d.settings.brightness;
                    document.getElementById('brightness-value').textContent = d.settings.brightness;
                }
                const viewInfo = viewsData.find(v => v.id === newView);
                const viewName = viewInfo ? viewInfo.name : 'View ' + newView;
                document.getElementById('status').innerHTML = `
                    <div class="status-row"><span class="status-label">Board</span><span class="status-value">${d.board}</span></div>
                    <div class="status-row"><span class="status-label">Version</span><span class="status-value">${d.version}</span></div>
                    <div class="status-row"><span class="status-label">Current View</span><span class="status-value" style="color:#ffe000">${viewName}</span></div>
                    <div class="status-row"><span class="status-label">Uptime</span><span class="status-value">${Math.floor(d.uptime/60)}m ${d.uptime%60}s</span></div>
                    <div class="status-row"><span class="status-label">WiFi</span><span class="status-value">${d.wifi_connected ? d.ssid + ' (' + d.rssi + ' dBm)' : 'AP Mode'}</span></div>
                    <div class="status-row"><span class="status-label">IP</span><span class="status-value">${d.ip}</span></div>
                    <div class="status-row"><span class="status-label">Free Heap</span><span class="status-value">${(d.free_heap/1024).toFixed(1)} KB</span></div>
                `;
                // SD Card status
                const sd = d.sd_card;
                if (sd && sd.mounted) {
                    const usedPct = ((sd.used_mb / sd.total_mb) * 100).toFixed(0);
                    document.getElementById('sd-status').innerHTML = `
                        <div class="status-row"><span class="status-label">Status</span><span class="status-value">Mounted <span class="badge badge-green">OK</span></span></div>
                        <div class="status-row"><span class="status-label">Capacity</span><span class="status-value">${sd.total_mb.toFixed(0)} MB</span></div>
                        <div class="status-row"><span class="status-label">Used</span><span class="status-value">${sd.used_mb.toFixed(1)} MB (${usedPct}%)</span></div>
                        <div class="progress-bar"><div class="progress-fill" style="width:${usedPct}%"></div></div>
                        <div class="status-row"><span class="status-label">Settings</span><span class="status-value">${sd.settings_loaded ? '<span class="badge badge-green">Loaded</span>' : '<span class="badge badge-yellow">Defaults</span>'}</span></div>
                        <div class="status-row"><span class="status-label">Log Size</span><span class="status-value">${(sd.log_size/1024).toFixed(1)} KB</span></div>
                    `;
                } else {
                    document.getElementById('sd-status').innerHTML = `
                        <div class="status-row"><span class="status-label">Status</span><span class="status-value">Not Mounted <span class="badge badge-red">N/A</span></span></div>
                        <div class="sd-info">Insert SD card and restart to enable persistent settings</div>
                    `;
                }
                // TfNSW status
                const tfnsw = d.tfnsw;
                if (tfnsw) {
                    const statusBadge = tfnsw.has_api_key ?
                        (tfnsw.status === 'Live' ? '<span class="badge badge-green">Live</span>' : '<span class="badge badge-yellow">' + tfnsw.status + '</span>') :
                        '<span class="badge badge-red">No Key</span>';
                    let depHtml = '';
                    if (tfnsw.departures && tfnsw.departures.length > 0) {
                        depHtml = '<div style="margin-top:10px;font-size:12px;color:#888">Next departures:</div>';
                        tfnsw.departures.forEach(d => {
                            const rtBadge = d.realtime ? '<span style="color:#4caf50">●</span>' : '';
                            depHtml += `<div class="status-row"><span class="status-label">${d.destination}</span><span class="status-value">${rtBadge} ${d.mins}</span></div>`;
                        });
                    }
                    document.getElementById('tfnsw-status').innerHTML = `
                        <div class="status-row"><span class="status-label">Status</span><span class="status-value">${statusBadge}</span></div>
                        <div class="status-row"><span class="status-label">API Key</span><span class="status-value">${tfnsw.has_api_key ? 'Configured' : 'Not set'}</span></div>
                        <div class="status-row"><span class="status-label">Station</span><span class="status-value">${tfnsw.station || 'Victoria Cross'}</span></div>
                        ${depHtml}
                    `;
                } else {
                    document.getElementById('tfnsw-status').innerHTML = '<div class="status-row"><span class="status-label">Status</span><span class="status-value">Not initialized</span></div>';
                }
            } catch(e) { document.getElementById('status').innerHTML = '<p style="color:#f44336">Error loading status</p>'; }
        }
        function updateViewButtons() {
            if (!viewsLoaded) return;
            document.querySelectorAll('#view-btns .view-btn').forEach(btn => {
                btn.classList.toggle('active', parseInt(btn.dataset.view) === currentView);
            });
            // Update current view name display
            const view = viewsData.find(v => v.id === currentView);
            if (view) {
                document.getElementById('current-view-name').textContent = view.name;
            }
        }
        function updateThemeButtons() {
            document.querySelectorAll('.color-btn').forEach(btn => {
                btn.classList.toggle('active', parseInt(btn.dataset.color) === currentTheme);
            });
        }
        async function setView(viewId) {
            await fetch('/api/display', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({command: 'scene', scene: viewId}) });
            currentView = viewId;
            updateViewButtons();
            // Refresh views to get updated active state
            setTimeout(fetchViews, 300);
        }
        async function setTheme(color) {
            await fetch('/api/display', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({command: 'theme', color: color}) });
            currentTheme = color;
            updateThemeButtons();
        }
        async function sendCmd(cmd) {
            await fetch('/api/display', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({command: cmd}) });
        }
        async function setBrightness() {
            const b = document.getElementById('brightness').value;
            await fetch('/api/display', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({command: 'brightness', level: parseInt(b)}) });
        }
        async function sysCmd(cmd) {
            if (!confirm('Are you sure?')) return;
            await fetch('/api/system', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({command: cmd}) });
        }
        async function settingsAction(action) {
            if (!confirm('Are you sure?')) return;
            const res = await fetch('/api/settings', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({action: action}) });
            const data = await res.json();
            alert(data.message);
            fetchStatus();
        }
        document.getElementById('wifi-form').addEventListener('submit', async (e) => {
            e.preventDefault();
            const ssid = document.getElementById('wifi-ssid').value;
            const pass = document.getElementById('wifi-pass').value;
            await fetch('/api/wifi', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({ssid, password: pass}) });
            alert('Credentials saved. Device will restart.');
        });
        document.getElementById('apikey-form').addEventListener('submit', async (e) => {
            e.preventDefault();
            const apikey = document.getElementById('apikey').value;
            if (!apikey) { alert('Please enter an API key'); return; }
            const res = await fetch('/api/tfnsw', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({action: 'set_key', api_key: apikey}) });
            const data = await res.json();
            alert(data.message);
            document.getElementById('apikey').value = '';
            fetchStatus();
        });
        async function clearApiKey() {
            if (!confirm('Clear API key?')) return;
            const res = await fetch('/api/tfnsw', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({action: 'clear_key'}) });
            const data = await res.json();
            alert(data.message);
            fetchStatus();
        }
        async function refreshDepartures() {
            const res = await fetch('/api/tfnsw', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({action: 'refresh'}) });
            const data = await res.json();
            fetchStatus();
        }
        let ledAutoMode = true;
        let currentLedColor = 0;
        function updateLedButtons() {
            document.querySelectorAll('#led-grid .color-btn').forEach(btn => {
                btn.classList.toggle('active', !ledAutoMode && parseInt(btn.dataset.led) === currentLedColor);
            });
            document.getElementById('led-auto-btn').classList.toggle('btn-primary', ledAutoMode);
            document.getElementById('led-auto-btn').classList.toggle('btn-secondary', !ledAutoMode);
        }
        async function setLed(color) {
            await fetch('/api/led', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({action: 'set_color', color: color}) });
            ledAutoMode = false;
            currentLedColor = color;
            updateLedButtons();
        }
        async function setLedAuto() {
            await fetch('/api/led', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({action: 'auto'}) });
            ledAutoMode = true;
            updateLedButtons();
        }
        async function setLedOff() {
            await fetch('/api/led', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({action: 'off'}) });
            ledAutoMode = false;
            currentLedColor = 0;
            updateLedButtons();
        }
        async function fetchDebug() {
            try {
                const res = await fetch('/api/debug');
                const d = await res.json();
                const t = d.tfnsw || {};
                const errCtx = t.parse_error_context || 'None';
                const respStart = (t.response_start || '').substring(0, 50);
                const respEnd = (t.response_end || '').substring(0, 50);
                document.getElementById('debug-info').innerHTML = `
                    <div class="status-row"><span class="status-label">Free Heap</span><span class="status-value">${(d.free_heap/1024).toFixed(1)} KB</span></div>
                    <div class="status-row"><span class="status-label">Min Free Heap</span><span class="status-value">${(d.min_free_heap/1024).toFixed(1)} KB</span></div>
                    <div class="status-row"><span class="status-label">Largest Block</span><span class="status-value">${(d.largest_free_block/1024).toFixed(1)} KB</span></div>
                    <div class="status-row"><span class="status-label">API Status</span><span class="status-value">${t.status}</span></div>
                    <div class="status-row"><span class="status-label">Last Response</span><span class="status-value">${(t.last_response_size/1024).toFixed(1)} KB</span></div>
                    <div class="status-row"><span class="status-label">Buffer Size</span><span class="status-value">${(t.buffer_size/1024).toFixed(0)} KB ${t.buffer_overflow ? '<span class="badge badge-red">OVERFLOW</span>' : ''}</span></div>
                    <div class="status-row"><span class="status-label">Heap Before Parse</span><span class="status-value">${(t.heap_before_parse/1024).toFixed(1)} KB</span></div>
                    <div class="status-row"><span class="status-label">Heap After Parse</span><span class="status-value">${(t.heap_after_parse/1024).toFixed(1)} KB</span></div>
                    <div class="status-row"><span class="status-label">Parse Stats</span><span class="status-value">${t.parse_success_count} OK / ${t.parse_fail_count} fail</span></div>
                    <div class="status-row"><span class="status-label">Parse Error</span><span class="status-value" style="font-size:11px;word-break:break-all">${errCtx}</span></div>
                    <div class="status-row"><span class="status-label">Response Start</span><span class="status-value" style="font-size:10px;word-break:break-all">${respStart}...</span></div>
                    <div class="status-row"><span class="status-label">Response End</span><span class="status-value" style="font-size:10px;word-break:break-all">...${respEnd}</span></div>
                    <div class="status-row"><span class="status-label">Data Status</span><span class="status-value">${t.northbound_count}N/${t.southbound_count}S ${t.is_stale ? '<span class="badge badge-yellow">STALE</span>' : ''} ${t.is_cached_fallback ? '<span class="badge badge-yellow">CACHED</span>' : ''}</span></div>
                    <div class="status-row"><span class="status-label">Error Msg</span><span class="status-value" style="font-size:11px">${t.error_message || 'None'}</span></div>
                `;
            } catch(e) { document.getElementById('debug-info').innerHTML = '<p style="color:#f44336">Error loading debug info</p>'; }
        }
        fetchViews();
        fetchStatus();
        fetchDebug();
        setInterval(fetchStatus, 5000);
        setInterval(fetchDebug, 10000);
    </script>
</body>
</html>
)rawliteral";

// ============================================================================
// Request Handlers
// ============================================================================

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
    return ESP_OK;
}

static esp_err_t api_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "board", BOARD_NAME);
    cJSON_AddStringToObject(root, "version", FIRMWARE_VERSION);
    cJSON_AddNumberToObject(root, "uptime", esp_timer_get_time() / 1000000);
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_is_connected());
    cJSON_AddStringToObject(root, "ip", wifi_get_ip());
    cJSON_AddStringToObject(root, "ssid", wifi_get_ssid());
    cJSON_AddNumberToObject(root, "rssi", wifi_get_rssi());
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "view", lcd_get_current_view());
    cJSON_AddNumberToObject(root, "scene", lcd_get_current_scene());  // Legacy
    cJSON_AddNumberToObject(root, "theme_color", lcd_get_theme_accent());

    // Storage info (SD card disabled)
    cJSON *storage = cJSON_CreateObject();
    cJSON_AddBoolToObject(storage, "mounted", false);
    cJSON_AddStringToObject(storage, "type", "nvs");
    cJSON_AddItemToObject(root, "storage", storage);

    // Current settings summary
    const device_settings_t* cfg = settings_get();
    cJSON *settings = cJSON_CreateObject();
    cJSON_AddNumberToObject(settings, "brightness", cfg->brightness);
    cJSON_AddNumberToObject(settings, "default_scene", cfg->default_scene);
    cJSON_AddStringToObject(settings, "destination", cfg->destination);
    cJSON_AddItemToObject(root, "settings", settings);

    // TfNSW status
    cJSON *tfnsw = cJSON_CreateObject();
    cJSON_AddBoolToObject(tfnsw, "has_api_key", tfnsw_has_api_key());
    cJSON_AddStringToObject(tfnsw, "status", tfnsw_status_to_string(tfnsw_get_status()));

    // Add current departures if available
    tfnsw_departures_t deps;
    tfnsw_get_current_departures(&deps);
    if (deps.count > 0) {
        cJSON_AddStringToObject(tfnsw, "station", deps.station_name);
        cJSON *departures = cJSON_CreateArray();
        for (int i = 0; i < deps.count && i < 3; i++) {
            cJSON *dep = cJSON_CreateObject();
            cJSON_AddStringToObject(dep, "destination", deps.departures[i].destination);
            char mins_str[16];
            tfnsw_format_departure_time(deps.departures[i].mins_to_departure, mins_str, sizeof(mins_str));
            cJSON_AddStringToObject(dep, "mins", mins_str);
            cJSON_AddBoolToObject(dep, "realtime", deps.departures[i].is_realtime);
            cJSON_AddItemToArray(departures, dep);
        }
        cJSON_AddItemToObject(tfnsw, "departures", departures);
    }
    cJSON_AddItemToObject(root, "tfnsw", tfnsw);

    const char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));

    free((void*)json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_views_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *views = cJSON_CreateArray();

    // Get current view info
    view_id_t current = lcd_get_current_view();
    const view_config_t* current_config = lcd_get_view_config(current);
    cJSON_AddNumberToObject(root, "current", current);
    cJSON_AddStringToObject(root, "current_name", current_config ? current_config->name : "Unknown");

    // Get all views from the view registry
    uint8_t view_count = lcd_get_view_count();
    for (uint8_t i = 0; i < view_count; i++) {
        const view_config_t* config = lcd_get_view_config((view_id_t)i);
        if (config) {
            cJSON *view = cJSON_CreateObject();
            cJSON_AddNumberToObject(view, "id", config->id);
            cJSON_AddStringToObject(view, "name", config->name);
            cJSON_AddStringToObject(view, "header", config->header_title);
            cJSON_AddNumberToObject(view, "accent_color", config->accent_color);
            cJSON_AddNumberToObject(view, "led_color", config->led_color);
            cJSON_AddStringToObject(view, "data_source", config->data_source == VIEW_DATA_REALTIME ? "realtime" : "static");
            cJSON_AddBoolToObject(view, "enabled", config->enabled);
            cJSON_AddBoolToObject(view, "active", i == current);
            cJSON_AddItemToArray(views, view);
        }
    }
    cJSON_AddItemToObject(root, "views", views);

    const char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));

    free((void*)json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_display_handler(httpd_req_t *req)
{
    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(root, "command");
    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command");
        return ESP_FAIL;
    }

    const char *command = cmd_item->valuestring;
    ESP_LOGI(TAG, "Display command: %s", command);

    if (display_callback) {
        display_callback(command, buf);
    }

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

static esp_err_t api_system_handler(httpd_req_t *req)
{
    char buf[128];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(root, "command");
    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command");
        return ESP_FAIL;
    }

    const char *command = cmd_item->valuestring;
    ESP_LOGI(TAG, "System command: %s", command);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");

    if (strcmp(command, "restart") == 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (strcmp(command, "reset_wifi") == 0) {
        wifi_clear_credentials();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (system_callback) {
        system_callback(command);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_wifi_handler(httpd_req_t *req)
{
    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");

    if (!ssid_item || !cJSON_IsString(ssid_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    const char *ssid = ssid_item->valuestring;
    const char *password = pass_item && cJSON_IsString(pass_item) ? pass_item->valuestring : "";

    ESP_LOGI(TAG, "Saving WiFi credentials for: %s", ssid);
    wifi_save_credentials(ssid, password);

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Credentials saved. Restarting...\"}");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// ============================================================================
// Debug API Handler
// ============================================================================

static esp_err_t api_debug_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    // System memory info
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "largest_free_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // TfNSW debug info
    tfnsw_debug_info_t dbg;
    tfnsw_get_debug_info(&dbg);

    cJSON *tfnsw = cJSON_CreateObject();
    cJSON_AddNumberToObject(tfnsw, "last_response_size", dbg.last_response_size);
    cJSON_AddNumberToObject(tfnsw, "heap_before_parse", dbg.last_parse_heap_before);
    cJSON_AddNumberToObject(tfnsw, "heap_after_parse", dbg.last_parse_heap_after);
    cJSON_AddNumberToObject(tfnsw, "parse_error_offset", dbg.parse_error_offset);
    cJSON_AddStringToObject(tfnsw, "parse_error_context", dbg.parse_error_context);
    cJSON_AddStringToObject(tfnsw, "response_start", dbg.response_start);
    cJSON_AddStringToObject(tfnsw, "response_end", dbg.response_end);
    cJSON_AddNumberToObject(tfnsw, "fetch_count", dbg.fetch_count);
    cJSON_AddNumberToObject(tfnsw, "parse_success_count", dbg.parse_success_count);
    cJSON_AddNumberToObject(tfnsw, "parse_fail_count", dbg.parse_fail_count);
    cJSON_AddNumberToObject(tfnsw, "buffer_size", dbg.buffer_size);
    cJSON_AddBoolToObject(tfnsw, "buffer_overflow", dbg.buffer_overflow);
    cJSON_AddStringToObject(tfnsw, "status", tfnsw_status_to_string(tfnsw_get_status()));

    // Current departure data status
    tfnsw_dual_departures_t deps;
    tfnsw_get_current_dual_departures(&deps);
    cJSON_AddNumberToObject(tfnsw, "northbound_count", deps.northbound_count);
    cJSON_AddNumberToObject(tfnsw, "southbound_count", deps.southbound_count);
    cJSON_AddBoolToObject(tfnsw, "is_stale", deps.is_stale);
    cJSON_AddBoolToObject(tfnsw, "is_cached_fallback", deps.is_cached_fallback);
    cJSON_AddNumberToObject(tfnsw, "data_age_seconds", deps.data_age_seconds);
    cJSON_AddStringToObject(tfnsw, "error_message", deps.error_message);

    cJSON_AddItemToObject(root, "tfnsw", tfnsw);

    const char *json = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));

    free((void*)json);
    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================================
// TfNSW API Handler
// ============================================================================

static esp_err_t api_tfnsw_handler(httpd_req_t *req)
{
    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!action || !cJSON_IsString(action)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing action");
        return ESP_FAIL;
    }

    const char *action_str = action->valuestring;
    httpd_resp_set_type(req, "application/json");

    if (strcmp(action_str, "set_key") == 0) {
        cJSON *api_key = cJSON_GetObjectItem(root, "api_key");
        if (!api_key || !cJSON_IsString(api_key) || strlen(api_key->valuestring) == 0) {
            cJSON_Delete(root);
            httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Invalid API key\"}");
            return ESP_OK;
        }

        esp_err_t ret = tfnsw_set_api_key(api_key->valuestring);
        if (ret == ESP_OK) {
            // Notify main loop to start/restart fetch and show confirmation
            if (api_key_callback) {
                api_key_callback();
            }
            httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"API key saved. Fetching departures...\"}");
        } else {
            httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to save API key\"}");
        }
    } else if (strcmp(action_str, "clear_key") == 0) {
        tfnsw_clear_api_key();
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"API key cleared\"}");
    } else if (strcmp(action_str, "refresh") == 0) {
        tfnsw_force_refresh();
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Refresh requested\"}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Unknown action\"}");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================================
// LED API Handler
// ============================================================================

static esp_err_t api_led_handler(httpd_req_t *req)
{
    char buf[128];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!action || !cJSON_IsString(action)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing action");
        return ESP_FAIL;
    }

    const char *action_str = action->valuestring;
    httpd_resp_set_type(req, "application/json");

    if (strcmp(action_str, "set_color") == 0) {
        cJSON *color = cJSON_GetObjectItem(root, "color");
        if (color && cJSON_IsNumber(color)) {
            uint32_t hex_color = (uint32_t)color->valueint;
            rgb_led_set_hex(hex_color);
            ESP_LOGI(TAG, "LED color set to 0x%06X", (unsigned int)hex_color);
            httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"LED color set\"}");
        } else {
            httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Invalid color\"}");
        }
    } else if (strcmp(action_str, "auto") == 0) {
        // Re-enable auto mode by triggering view-based LED update
        view_id_t view = lcd_get_current_view();
        const view_config_t* config = lcd_get_view_config(view);
        if (config && view != VIEW_STATUS_INFO) {
            rgb_led_set_hex(config->led_color);
        } else {
            rgb_led_set_status(rgb_led_get_status());
        }
        ESP_LOGI(TAG, "LED set to auto mode for view %d", view);
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"LED auto mode enabled\"}");
    } else if (strcmp(action_str, "off") == 0) {
        rgb_led_set_hex(0);  // Use set_hex to enable manual mode and turn off
        ESP_LOGI(TAG, "LED turned off");
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"LED turned off\"}");
    } else {
        httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Unknown action\"}");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================================
// Settings API Handler
// ============================================================================

static esp_err_t api_settings_get_handler(httpd_req_t *req)
{
    const device_settings_t* cfg = settings_get();

    cJSON *root = cJSON_CreateObject();

    // Display settings
    cJSON *display = cJSON_CreateObject();
    cJSON_AddNumberToObject(display, "theme_color", cfg->theme_color);
    cJSON_AddNumberToObject(display, "brightness", cfg->brightness);
    cJSON_AddNumberToObject(display, "default_scene", cfg->default_scene);
    cJSON_AddItemToObject(root, "display", display);

    // Metro settings
    cJSON *metro = cJSON_CreateObject();
    cJSON_AddStringToObject(metro, "destination", cfg->destination);
    cJSON_AddStringToObject(metro, "calling", cfg->calling_stations);
    cJSON_AddStringToObject(metro, "time", cfg->departure_time);
    cJSON_AddNumberToObject(metro, "mins", cfg->departure_mins);
    cJSON_AddStringToObject(metro, "next_dest", cfg->next_dest);
    cJSON_AddStringToObject(metro, "next_time", cfg->next_time);
    cJSON_AddStringToObject(metro, "next2_dest", cfg->next2_dest);
    cJSON_AddStringToObject(metro, "next2_time", cfg->next2_time);
    cJSON_AddItemToObject(root, "metro", metro);

    // High speed settings
    cJSON *highspeed = cJSON_CreateObject();
    cJSON_AddStringToObject(highspeed, "destination", cfg->hs_destination);
    cJSON_AddStringToObject(highspeed, "calling", cfg->hs_calling);
    cJSON_AddStringToObject(highspeed, "time", cfg->hs_time);
    cJSON_AddNumberToObject(highspeed, "mins", cfg->hs_mins);
    cJSON_AddItemToObject(root, "highspeed", highspeed);

    cJSON_AddBoolToObject(root, "loaded_from_sd", cfg->loaded);

    const char *json = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));

    free((void*)json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_settings_post_handler(httpd_req_t *req)
{
    char buf[512];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!action || !cJSON_IsString(action)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing action");
        return ESP_FAIL;
    }

    const char *action_str = action->valuestring;

    if (strcmp(action_str, "reset") == 0) {
        settings_reset();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Settings reset to defaults\"}");
    } else if (strcmp(action_str, "save") == 0) {
        esp_err_t ret = settings_save();
        httpd_resp_set_type(req, "application/json");
        if (ret == ESP_OK) {
            httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Settings saved\"}");
        } else {
            httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to save settings\"}");
        }
    } else if (strcmp(action_str, "clear_log") == 0) {
        esp_err_t ret = log_clear();
        httpd_resp_set_type(req, "application/json");
        if (ret == ESP_OK) {
            httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Log cleared\"}");
        } else {
            httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to clear log\"}");
        }
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================================
// Server Management
// ============================================================================

esp_err_t webserver_start(void)
{
    if (server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.lru_purge_enable = true;
    config.stack_size = 8192;  // Increase stack for cJSON operations
    config.max_uri_handlers = 16;  // Increase from default 8 to support all endpoints

    ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server");
        return ESP_FAIL;
    }

    // Register URI handlers
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler
    };
    httpd_register_uri_handler(server, &root_uri);

    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_handler
    };
    httpd_register_uri_handler(server, &status_uri);

    httpd_uri_t views_uri = {
        .uri = "/api/views",
        .method = HTTP_GET,
        .handler = api_views_handler
    };
    httpd_register_uri_handler(server, &views_uri);

    httpd_uri_t display_uri = {
        .uri = "/api/display",
        .method = HTTP_POST,
        .handler = api_display_handler
    };
    httpd_register_uri_handler(server, &display_uri);

    httpd_uri_t system_uri = {
        .uri = "/api/system",
        .method = HTTP_POST,
        .handler = api_system_handler
    };
    httpd_register_uri_handler(server, &system_uri);

    httpd_uri_t wifi_uri = {
        .uri = "/api/wifi",
        .method = HTTP_POST,
        .handler = api_wifi_handler
    };
    httpd_register_uri_handler(server, &wifi_uri);

    httpd_uri_t settings_get_uri = {
        .uri = "/api/settings",
        .method = HTTP_GET,
        .handler = api_settings_get_handler
    };
    httpd_register_uri_handler(server, &settings_get_uri);

    httpd_uri_t settings_post_uri = {
        .uri = "/api/settings",
        .method = HTTP_POST,
        .handler = api_settings_post_handler
    };
    httpd_register_uri_handler(server, &settings_post_uri);

    httpd_uri_t tfnsw_uri = {
        .uri = "/api/tfnsw",
        .method = HTTP_POST,
        .handler = api_tfnsw_handler
    };
    httpd_register_uri_handler(server, &tfnsw_uri);

    httpd_uri_t led_uri = {
        .uri = "/api/led",
        .method = HTTP_POST,
        .handler = api_led_handler
    };
    httpd_register_uri_handler(server, &led_uri);

    httpd_uri_t debug_uri = {
        .uri = "/api/debug",
        .method = HTTP_GET,
        .handler = api_debug_handler
    };
    httpd_register_uri_handler(server, &debug_uri);

    ESP_LOGI(TAG, "Web server started");
    return ESP_OK;
}

esp_err_t webserver_stop(void)
{
    if (server == NULL) {
        return ESP_OK;
    }

    httpd_stop(server);
    server = NULL;
    ESP_LOGI(TAG, "Web server stopped");
    return ESP_OK;
}

bool webserver_is_running(void)
{
    return server != NULL;
}

void webserver_set_display_callback(display_cmd_cb_t cb)
{
    display_callback = cb;
}

void webserver_set_system_callback(system_cmd_cb_t cb)
{
    system_callback = cb;
}

void webserver_set_api_key_callback(api_key_set_cb_t cb)
{
    api_key_callback = cb;
}
