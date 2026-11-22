#!/usr/bin/env node

/**
 * Small development-only controller for the DAWg WebView2 page.
 *
 * Start DAWg with:
 *   $env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = '--remote-debugging-port=9222'
 *   build\app\DAWg_artefacts\Debug\DAWg.exe --dev http://127.0.0.1:5173
 *
 * Then run:
 *   node tools/dawg-cdp.mjs verify-graph
 *
 * Node 22+ is required for the built-in WebSocket implementation.
 */

const port = Number(process.env.DAWG_CDP_PORT ?? 9222);
const baseUrl = `http://127.0.0.1:${port}`;

async function target() {
  const response = await fetch(`${baseUrl}/json/list`);
  if (!response.ok) throw new Error(`CDP discovery failed: HTTP ${response.status}`);
  const pages = await response.json();
  const page = pages.find(
    (candidate) => candidate.type === 'page' && /https?:\/\/(?:127\.0\.0\.1|localhost):5173/.test(candidate.url),
  );
  if (!page) throw new Error('No DAWg WebView page found on the CDP endpoint');
  return page;
}

async function connect() {
  const page = await target();
  const socket = new WebSocket(page.webSocketDebuggerUrl);
  let nextId = 0;
  const pending = new Map();

  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    const handlers = pending.get(message.id);
    if (!handlers) return;
    pending.delete(message.id);
    if (message.error) handlers.reject(new Error(message.error.message));
    else handlers.resolve(message.result);
  });

  await new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve, { once: true });
    socket.addEventListener('error', reject, { once: true });
  });

  const command = (method, params = {}) =>
    new Promise((resolve, reject) => {
      const id = ++nextId;
      pending.set(id, { resolve, reject });
      socket.send(JSON.stringify({ id, method, params }));
    });

  const evaluate = async (expression) => {
    const result = await command('Runtime.evaluate', {
      expression,
      returnByValue: true,
      awaitPromise: true,
    });
    if (result.exceptionDetails) {
      throw new Error(result.exceptionDetails.text ?? 'Browser evaluation failed');
    }
    return result.result?.value;
  };

  return {
    page,
    evaluate,
    close: () => socket.close(),
  };
}

const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

async function clickByTitle(connection, title) {
  const clicked = await connection.evaluate(`(() => {
    const button = [...document.querySelectorAll('button')].find((element) => element.title === ${JSON.stringify(title)});
    if (!button) return false;
    button.click();
    return true;
  })()`);
  if (!clicked) throw new Error(`Could not find button titled "${title}"`);
}

async function freshGraph(connection) {
  await connection.evaluate(`(() => {
    const button = [...document.querySelectorAll('button')].find((element) => element.innerText.includes('New project'));
    if (!button) return false;
    button.click();
    return true;
  })()`);
  await sleep(500);
  await clickByTitle(connection, 'Node graph (Tab)');
  await sleep(500);
}

async function inspectGraph(connection) {
  return connection.evaluate(`(() => {
    const nodes = [...document.querySelectorAll('.react-flow__node')];
    const node = (name) => nodes.find((element) => element.querySelector('.graph-node-name')?.textContent?.trim() === name);
    const count = (element, selector) => element ? element.querySelectorAll(selector).length : null;
    const gain = node('Gain');
    const lfo = node('LFO');
    return {
      page: location.href,
      graphNodes: nodes.length,
      gainModulationInputs: count(gain, '.graph-port.modulation'),
      gainTargetSelectors: count(gain, '.graph-mod-target'),
      lfoPresent: Boolean(lfo),
      lfoModulationOutputs: count(lfo, '.graph-port.modulation'),
    };
  })()`);
}

async function inspectLfoEditor(connection) {
  return connection.evaluate(`(() => ({
    lfoEditorVisible: Boolean(document.querySelector('.lfo-shape-canvas')),
    controlPoints: document.querySelectorAll('.lfo-shape-canvas .lfo-point').length,
    tensionHandles: document.querySelectorAll('.lfo-shape-canvas .lfo-tension-handle').length,
    pointGeometry: [...document.querySelectorAll('.lfo-shape-canvas .lfo-point')].map((element) => ({
      tag: element.tagName.toLowerCase(),
      rx: element.getAttribute('rx'),
      ry: element.getAttribute('ry'),
    })),
  }))()`);
}

async function main() {
  const action = process.argv[2] ?? 'inspect';
  const connection = await connect();
  try {
    if (action === 'fresh-graph' || action === 'verify-graph') await freshGraph(connection);
    if (action === 'inspect' || action === 'fresh-graph' || action === 'verify-graph') {
      console.log(JSON.stringify(await inspectGraph(connection), null, 2));
    } else if (action === 'inspect-lfo') {
      console.log(JSON.stringify(await inspectLfoEditor(connection), null, 2));
    } else if (action === 'eval') {
      const expression = process.env.DAWG_CDP_EXPR;
      if (!expression) throw new Error('Set DAWG_CDP_EXPR for the eval action');
      console.log(JSON.stringify(await connection.evaluate(expression), null, 2));
    } else {
      throw new Error(`Unknown action "${action}". Use inspect, fresh-graph, verify-graph, or inspect-lfo.`);
    }
  } finally {
    connection.close();
  }
}

main().catch((error) => {
  console.error(`dawg-cdp: ${error.message}`);
  process.exitCode = 1;
});
