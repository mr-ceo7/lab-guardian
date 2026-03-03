import { BluetoothSerial } from '@e-is/capacitor-bluetooth-serial';
import { LocalNotifications } from '@capacitor/local-notifications';

/**
 * Bluetooth service for Lab Guardian.
 * Uses event-based startNotifications for reliable data reception.
 */

let dataCallback = null;
let alertCallback = null;
let connectionCallback = null;
let phonesCallback = null;
let ackCallback = null;
let connectedAddress = null;

/**
 * Scan for Bluetooth devices.
 */
export async function listDevices() {
  try {
    const state = await BluetoothSerial.isEnabled();
    if (!state.enabled) {
      await BluetoothSerial.enable();
    }
  } catch (e) {
    console.log('BT enable:', e);
  }
  try {
    const result = await BluetoothSerial.scan();
    return result.devices || [];
  } catch (e) {
    console.error('Scan error:', e);
    return [];
  }
}

/**
 * Connect and start listening for data via events.
 */
export async function connectDevice(address) {
  await BluetoothSerial.connect({ address });
  connectedAddress = address;
  if (connectionCallback) connectionCallback(true);

  // Use event-based listener instead of polling
  await BluetoothSerial.startNotifications({
    address: connectedAddress,
    delimiter: '\n'
  });

  BluetoothSerial.addListener('onRead', (result) => {
    if (result.value && result.value.trim()) {
      handleIncoming(result.value.trim());
    }
  });
}

/**
 * Disconnect.
 */
export async function disconnectDevice() {
  try {
    if (connectedAddress) {
      await BluetoothSerial.stopNotifications({ address: connectedAddress });
      await BluetoothSerial.removeAllListeners();
      await BluetoothSerial.disconnect({ address: connectedAddress });
    }
  } catch (e) {
    console.log('Disconnect:', e);
  }
  connectedAddress = null;
  if (connectionCallback) connectionCallback(false);
}

/**
 * Send a command to Arduino.
 */
export async function sendCommand(cmd) {
  if (!connectedAddress) return false;
  try {
    await BluetoothSerial.write({
      address: connectedAddress,
      value: cmd + '\n'
    });
    return true;
  } catch (e) {
    console.error('Send error:', e);
    return false;
  }
}

export function onData(cb) { dataCallback = cb; }
export function onAlert(cb) { alertCallback = cb; }
export function onConnection(cb) { connectionCallback = cb; }
export function onPhones(cb) { phonesCallback = cb; }
export function onAck(cb) { ackCallback = cb; }

/**
 * Parse incoming JSON from Arduino.
 */
function handleIncoming(raw) {
  try {
    const obj = JSON.parse(raw);

    if ('mq2' in obj && dataCallback) {
      dataCallback(obj);
    }

    if ('alert' in obj) {
      if (alertCallback) alertCallback(obj.alert);
      if (obj.alert !== 'CLEAR') {
        fireNotification(obj.alert);
      }
    }

    if ('ack' in obj) {
      console.log('ACK:', obj.ack);
      if (ackCallback) ackCallback(obj.ack);
    }

    // Phone list: {"phones":["+254...","+254..."]}
    if ('phones' in obj && phonesCallback) {
      phonesCallback(obj.phones);
    }
  } catch (e) {
    // Not valid JSON — ignore partial lines
  }
}

/**
 * Fire a local notification.
 */
async function fireNotification(message) {
  try {
    await LocalNotifications.schedule({
      notifications: [{
        title: '⚠️ Lab Guardian Alert',
        body: message,
        id: Date.now() % 100000,
        schedule: { at: new Date(Date.now() + 100) }, // Schedule 100ms in future
        actionTypeId: 'ALARM_ACTIONS',
        extra: { type: 'alarm' },
        channelId: 'alarms' // Required for Android 8+
      }]
    });
  } catch (e) {
    console.error('Notification error:', e);
  }
}

/**
 * Setup notification permissions and action types.
 */
export async function setupNotifications() {
  try {
    const perm = await LocalNotifications.requestPermissions();
    console.log('Notification permissions:', perm);

    // Create channel for Android 8+ (required for notifications to show)
    await LocalNotifications.createChannel({
      id: 'alarms',
      name: 'Lab Guardian Alarms',
      description: 'Critical alerts for gas and fire detection',
      importance: 5, // 5 = MAXIMUM (heads-up notification)
      visibility: 1, // 1 = PUBLIC (shows on lock screen)
      vibration: true,
      lights: true,
      lightColor: '#00d2b4'
    });

    await LocalNotifications.registerActionTypes({
      types: [{
        id: 'ALARM_ACTIONS',
        actions: [{ id: 'silence', title: 'Silence Alarm' }]
      }]
    });

    LocalNotifications.addListener('localNotificationActionPerformed', async (action) => {
      if (action.actionId === 'silence') {
        await sendCommand('CMD:SILENCE');
      }
    });
  } catch (e) {
    console.error('Notification setup error:', e);
  }
}
