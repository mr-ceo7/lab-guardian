import { useState, useEffect, useRef } from 'react';
import {
  listDevices,
  connectDevice,
  disconnectDevice,
  sendCommand,
  onData,
  onAlert,
  onConnection,
  onPhones,
  onAck,
  setupNotifications
} from './bluetooth';
import './index.css';

function formatUptime(seconds) {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

function App() {
  const [connected, setConnected] = useState(false);
  const [connecting, setConnecting] = useState(false);
  const [devices, setDevices] = useState([]);
  const [scanning, setScanning] = useState(false);
  const [sensorData, setSensorData] = useState(null);
  const [alertHistory, setAlertHistory] = useState(() => {
    try {
      return JSON.parse(localStorage.getItem('alertHistory') || '[]');
    } catch { return []; }
  });
  const [phones, setPhones] = useState([]);
  const [newPhone, setNewPhone] = useState('');
  const [toast, setToast] = useState(null);

  const historyRef = useRef(alertHistory);
  historyRef.current = alertHistory;

  useEffect(() => {
    setupNotifications();

    onConnection((isConnected) => {
      setConnected(isConnected);
      if (!isConnected) {
        setSensorData(null);
        setPhones([]);
      }
    });

    onData((data) => {
      setSensorData(data);
    });

    onAlert((msg) => {
      if (msg !== 'CLEAR') {
        const entry = {
          message: msg,
          time: new Date().toLocaleTimeString(),
          date: new Date().toLocaleDateString()
        };
        const updated = [entry, ...historyRef.current].slice(0, 50);
        setAlertHistory(updated);
        localStorage.setItem('alertHistory', JSON.stringify(updated));
      }
    });

    onPhones((phoneList) => {
      setPhones(phoneList);
    });

    onAck((msg) => {
      const ackMessages = {
        'PHONE_SAVED': '✅ Contact saved to device!',
        'PHONES_CLEARED': '🗑️ All contacts cleared',
        'PHONE_FULL': '⚠️ Max 3 contacts reached',
        'SMS_SENDING': '📱 Sending test SMS...',
        'SMS_DONE': '✅ Test SMS sent!',
        'SILENCE_OK': '🔇 Buzzer silenced',
        'RESET_OK': '🔄 Alarm reset'
      };
      const text = ackMessages[msg] || msg;
      setToast(text);
      setTimeout(() => setToast(null), 3000);
    });

    // Auto-scan for paired devices on app load
    handleScanDevices();
  }, []);

  const handleScanDevices = async () => {
    setScanning(true);
    try {
      const devs = await listDevices();
      // Sort: named devices first (paired), then unknown
      devs.sort((a, b) => {
        const aName = a.name && a.name !== 'Unknown';
        const bName = b.name && b.name !== 'Unknown';
        if (aName && !bName) return -1;
        if (!aName && bName) return 1;
        return 0;
      });
      setDevices(devs);
    } catch (e) {
      console.error('Scan error:', e);
    }
    setScanning(false);
  };

  const handleConnect = async (address) => {
    setConnecting(true);
    try {
      await connectDevice(address);
      // Request phone list after connecting
      setTimeout(() => sendCommand('CMD:GETPHONES'), 1000);
    } catch (e) {
      console.error('Connect error:', e);
    }
    setConnecting(false);
  };

  const handleDisconnect = async () => {
    await disconnectDevice();
    setConnected(false);
  };

  const handleAddPhone = async () => {
    const num = newPhone.trim();
    if (num && num.startsWith('+') && num.length >= 10) {
      // Optimistic UI update — show immediately
      setPhones(prev => [...prev, num]);
      setNewPhone('');
      // Send to Arduino
      const ok = await sendCommand(`CMD:PHONE:${num}`);
      if (!ok) {
        // Revert if send failed
        setPhones(prev => prev.filter(p => p !== num));
      } else {
        // Re-request actual list from Arduino after a moment
        setTimeout(() => sendCommand('CMD:GETPHONES'), 1500);
      }
    }
  };

  const handleClearPhones = async () => {
    setPhones([]);
    await sendCommand('CMD:CLEARPHONES');
  };

  const handleClearHistory = () => {
    setAlertHistory([]);
    localStorage.removeItem('alertHistory');
  };

  const d = sensorData;

  return (
    <div>
      {/* Toast */}
      {toast && (
        <div className="toast">{toast}</div>
      )}

      {/* Header */}
      <div className="header">
        <h1>🛡️ Lab Guardian</h1>
        <div className="subtitle">Smart Lab Safety Monitor</div>
        <div className={`connection-badge ${connected ? 'connected' : 'disconnected'}`}>
          <span className="dot"></span>
          {connected ? 'Connected' : 'Disconnected'}
        </div>
      </div>

      {!connected ? (
        /* ==================== CONNECTION SCREEN ==================== */
        <div>
          <div className="card">
            <div className="card-title">📡 Bluetooth Connection</div>

            {scanning && devices.length === 0 && (
              <div className="loading">
                <div className="spinner"></div>
                Scanning for devices...
              </div>
            )}

            {connecting && (
              <div className="loading">
                <div className="spinner"></div>
                Connecting...
              </div>
            )}

            {devices.length > 0 && !connecting && (
              <>
                <div className="device-section-title">Paired Devices</div>
                <div className="device-list">
                  {devices.map((dev, i) => (
                    <div key={i} className="device-item" onClick={() => handleConnect(dev.address)}>
                      <div>
                        <div className="device-name">{dev.name || 'Unknown Device'}</div>
                        <div className="device-address">{dev.address}</div>
                      </div>
                      <span className="connect-arrow">→</span>
                    </div>
                  ))}
                </div>
              </>
            )}

            {devices.length === 0 && !scanning && (
              <p style={{ color: 'var(--text-dim)', fontSize: '0.8rem', textAlign: 'center', marginTop: 16 }}>
                No paired devices found.<br/>
                Pair your HC-05 in Android Bluetooth settings first.
              </p>
            )}

            <button className="btn btn-connect" onClick={handleScanDevices} disabled={scanning} style={{ marginTop: 12 }}>
              {scanning ? '🔍 Scanning...' : '🔄 Refresh Devices'}
            </button>
          </div>
        </div>
      ) : (
        /* ==================== DASHBOARD ==================== */
        <div>
          {/* Alarm Banner */}
          {d && d.alm === 1 && (
            <div className={`alarm-banner ${d.sil === 1 ? 'silenced' : ''}`}>
              <h3>{d.sil === 1 ? '🔇 Alarm Silenced' : '🚨 ALARM ACTIVE'}</h3>
              <p>{d.sil === 1 ? 'Buzzer off — LED still on' : 'Gas or hazard detected!'}</p>
            </div>
          )}

          {/* Sensor Tiles */}
          <div className="card">
            <div className="card-title">🔬 Sensors</div>
            <div className="sensor-grid">
              <div className={`sensor-tile ${d && d.mq2 ? 'triggered' : 'ok'}`}>
                <div className="icon">🔥</div>
                <div className="name">MQ-2 (Gas/Fire)</div>
                <div className="status">{d ? (d.mq2 ? 'DANGER' : 'SAFE') : '—'}</div>
              </div>
              <div className={`sensor-tile ${d && d.mq135 ? 'triggered' : 'ok'}`}>
                <div className="icon">💨</div>
                <div className="name">MQ-135 (Air)</div>
                <div className="status">{d ? (d.mq135 ? 'POOR' : 'GOOD') : '—'}</div>
              </div>
            </div>
          </div>

          {/* System Status */}
          <div className="card">
            <div className="card-title">📊 System Status</div>
            <div className="status-row">
              <span className="label">GSM Module</span>
              <span className={`value ${d && d.gsm ? 'good' : 'bad'}`}>
                {d ? (d.gsm ? '✓ Ready' : '✗ Down') : '—'}
              </span>
            </div>
            <div className="status-row">
              <span className="label">Signal Strength</span>
              <span className={`value ${d ? (d.sig > 10 ? 'good' : d.sig > 0 ? 'warn' : 'bad') : ''}`}>
                {d ? `${d.sig}/31` : '—'}
              </span>
            </div>
            <div className="status-row">
              <span className="label">Alarm</span>
              <span className={`value ${d && d.alm ? 'bad' : 'good'}`}>
                {d ? (d.alm ? (d.sil ? '🔇 Silenced' : '🚨 Active') : '✓ None') : '—'}
              </span>
            </div>
            <div className="status-row">
              <span className="label">Uptime</span>
              <span className="value">{d ? formatUptime(d.up) : '—'}</span>
            </div>
          </div>

          {/* Controls */}
          <div className="card">
            <div className="card-title">🎛️ Controls</div>
            <div className="btn-group">
              <button className="btn btn-silence" onClick={() => sendCommand('CMD:SILENCE')} disabled={!connected}>
                🔇 Silence
              </button>
              <button className="btn btn-reset" onClick={() => sendCommand('CMD:RESET')}>
                🔄 Reset
              </button>
              <button className="btn btn-test-sms" onClick={() => sendCommand('CMD:TEST_SMS')}>
                📱 Test SMS
              </button>
              <button className="btn btn-status" onClick={() => sendCommand('CMD:STATUS')}>
                📡 Refresh
              </button>
            </div>
          </div>

          {/* Emergency Contacts */}
          <div className="card">
            <div className="card-title">📞 Emergency Contacts</div>
            {phones.length > 0 ? (
              <div className="phone-list">
                {phones.map((phone, i) => (
                  <div key={i} className="phone-item">
                    <span className="phone-num">📱 {phone}</span>
                    <span className="phone-slot">#{i + 1}</span>
                  </div>
                ))}
              </div>
            ) : (
              <div className="history-empty">No contacts set. Add below.</div>
            )}
            <div className="phone-input-row">
              <input
                type="tel"
                className="phone-input"
                placeholder="+254xxxxxxxxx"
                value={newPhone}
                onChange={(e) => setNewPhone(e.target.value)}
                maxLength={15}
              />
              <button
                className="btn btn-add-phone"
                onClick={handleAddPhone}
                disabled={!newPhone.trim() || phones.length >= 3}
              >
                + Add
              </button>
            </div>
            {phones.length > 0 && (
              <button className="btn btn-clear-phones" onClick={handleClearPhones}>
                🗑️ Clear All Contacts
              </button>
            )}
            <p style={{ color: 'var(--text-dim)', fontSize: '0.7rem', marginTop: 8 }}>
              Max 3 contacts. Saved to device EEPROM (persists after reboot).
            </p>
          </div>

          {/* Alert History */}
          <div className="card">
            <div className="card-title">📋 Alert History</div>
            {alertHistory.length === 0 ? (
              <div className="history-empty">No alerts recorded yet</div>
            ) : (
              <>
                <div className="history-list">
                  {alertHistory.map((item, i) => (
                    <div key={i} className="history-item">
                      <span className="msg">⚠️ {item.message}</span>
                      <span className="time">{item.time}</span>
                    </div>
                  ))}
                </div>
                <button className="btn btn-clear-history" onClick={handleClearHistory}>
                  🗑️ Clear History
                </button>
              </>
            )}
          </div>

          {/* Disconnect */}
          <button className="btn btn-disconnect" onClick={handleDisconnect}>
            ⏏️ Disconnect
          </button>
        </div>
      )}
    </div>
  );
}

export default App;
