import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import { dirname, resolve } from "node:path";
import { chromium } from "../../software/web/node_modules/playwright/index.mjs";

// Feasibility probe only: no physical adapter or Chameleon is contacted.
const serviceUuid = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const rxUuid = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
const txUuid = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
const address = "09:08:07:06:05:04";
const html = `<!doctype html><button id="connect">Connect simulated Ultra</button><script>
document.querySelector('button').onclick = async () => {
  try {
    const device = await navigator.bluetooth.requestDevice({filters:[{services:['${serviceUuid}']}]});
    device.addEventListener('gattserverdisconnected', () => window.disconnected = true);
    const server = await device.gatt.connect();
    const service = await server.getPrimaryService('${serviceUuid}');
    const rx = await service.getCharacteristic('${rxUuid}');
    const tx = await service.getCharacteristic('${txUuid}');
    await tx.startNotifications();
    await rx.writeValueWithResponse(Uint8Array.of(0x11,0xef,0x03,0xe8,0,0,0,0,0x15,0));
    await rx.writeValueWithoutResponse(Uint8Array.of(0x11,0xef));
    window.result = {secureContext:isSecureContext,name:device.name,connected:server.connected,
      rxWrite:rx.properties.write,rxWriteWithoutResponse:rx.properties.writeWithoutResponse,
      txNotify:tx.properties.notify};
  } catch (error) {window.failure = String(error);}
};
</script>`;
const http = createServer((_request, response) => {
	response.writeHead(200, { "Content-Type": "text/html" });
	response.end(html);
});
await new Promise((resolve) => http.listen(0, "127.0.0.1", resolve));
let browser;
const operations = [];
const errors = [];
let selected = false;
try {
	browser = await chromium.launch({
		headless: true,
		channel: "chromium",
		args: ["--enable-features=WebBluetooth,WebBluetoothNewPermissionsBackend"],
	});
	const context = await browser.newContext();
	const page = await context.newPage();
	page.setDefaultTimeout(12000);
	console.log(`Chromium ${browser.version()}`);
	const cdp = await browser.newBrowserCDPSession();
	const deviceCdp = await context.newCDPSession(page);
	const send = (method, params) =>
		cdp.send(method, params).catch((error) => {
			errors.push(String(error));
		});
	await cdp.send("BluetoothEmulation.enable", {
		state: "powered-on",
		leSupported: true,
	});
	await deviceCdp.send("DeviceAccess.enable");
	deviceCdp.on("DeviceAccess.deviceRequestPrompted", (event) => {
		operations.push({ kind: "chooser", ...event });
		const device = event.devices.find((item) => item.id === address);
		if (device && !selected) {
			selected = true;
			void deviceCdp
				.send("DeviceAccess.selectPrompt", {
					id: event.id,
					deviceId: device.id,
				})
				.catch((error) => errors.push(String(error)));
		}
	});
	cdp.on("BluetoothEmulation.gattOperationReceived", (event) => {
		operations.push({ kind: "gatt", ...event });
		void send("BluetoothEmulation.simulateGATTOperationResponse", {
			...event,
			code: 0,
		});
	});
	cdp.on("BluetoothEmulation.characteristicOperationReceived", (event) => {
		operations.push({ kind: "characteristic", ...event });
		void send("BluetoothEmulation.simulateCharacteristicOperationResponse", {
			characteristicId: event.characteristicId,
			type: event.type,
			code: 0,
		});
	});
	cdp.on("BluetoothEmulation.descriptorOperationReceived", (event) => {
		operations.push({ kind: "descriptor", ...event });
		void send("BluetoothEmulation.simulateDescriptorOperationResponse", {
			descriptorId: event.descriptorId,
			type: event.type,
			code: 0,
			...(event.type === "read" ? { data: "AAA=" } : {}),
		});
	});
	await cdp.send("BluetoothEmulation.simulatePreconnectedPeripheral", {
		address,
		name: "ChameleonUltra simulated",
		manufacturerData: [],
		knownServiceUuids: [serviceUuid],
	});
	const { serviceId } = await cdp.send("BluetoothEmulation.addService", {
		address,
		serviceUuid,
	});
	await cdp.send("BluetoothEmulation.addCharacteristic", {
		serviceId,
		characteristicUuid: rxUuid,
		properties: { write: true, writeWithoutResponse: true },
	});
	const { characteristicId: txId } = await cdp.send(
		"BluetoothEmulation.addCharacteristic",
		{
			serviceId,
			characteristicUuid: txUuid,
			properties: { notify: true },
		},
	);
	await cdp.send("BluetoothEmulation.addDescriptor", {
		characteristicId: txId,
		descriptorUuid: "00002902-0000-1000-8000-00805f9b34fb",
	});
	await page.goto(`http://127.0.0.1:${http.address().port}`);
	await page.locator("#connect").click();
	try {
		await page.waitForFunction(() => window.result || window.failure);
	} catch (error) {
		console.error(JSON.stringify({ operations, errors }));
		throw error;
	}
	const result = await page.evaluate(() => ({
		...window.result,
		failure: window.failure,
	}));
	assert.equal(result.failure, undefined);
	assert.equal(result.secureContext, true);
	assert.equal(result.connected, true);
	assert.equal(result.rxWrite, true);
	assert.equal(result.rxWriteWithoutResponse, true);
	assert.equal(result.txNotify, true);
	assert.equal(
		operations.filter((event) => event.type === "subscribe-to-notifications")
			.length,
		1,
	);
	assert.deepEqual(
		operations
			.filter((event) => event.type === "write")
			.map((event) => event.writeType),
		["write-with-response", "write-without-response"],
	);
	await cdp.send("BluetoothEmulation.simulateGATTDisconnection", { address });
	await page.waitForFunction(() => window.disconnected);
	assert.deepEqual(errors, []);
	const evidence = {
		date: new Date().toISOString(),
		browser: browser.version(),
		result,
		operations,
		disconnectionEvent: true,
		physicalHardwareUsed: false,
		limits: [
			"No Chameleon protocol response or spontaneous notification injected.",
			"No real pairing, BLE fragmentation, radio timing or RF operation tested.",
		],
	};
	const output = resolve(
		process.argv[2] || ".cache/web-ble-study-20261106/probe-result.json",
	);
	await mkdir(dirname(output), { recursive: true });
	await writeFile(output, `${JSON.stringify(evidence, null, 2)}\n`);
	console.log(JSON.stringify(evidence, null, 2));
} finally {
	await browser?.close();
	await new Promise((resolve) => http.close(resolve));
}
