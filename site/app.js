/* Drives the gb-wasm module: ROM loading, frame loop, input, saves. */
var Module = {
  onRuntimeInitialized: function () {
    if (Module.ccall("wasm_boot", "number", [], []) !== 0) {
      document.getElementById("status").textContent = "Emulator init failed.";
      return;
    }
    document.getElementById("status").textContent =
      "WASM ready. Pick a .gb ROM to start.";
  }
};
(function () {
  "use strict";
  var canvas = document.getElementById("screen");
  var ctx = canvas.getContext("2d");
  var image = ctx.createImageData(160, 144);
  var statusEl = document.getElementById("status");
  var buttons = 0;
  var running = false;
  var booted = false;
  var speed = Number(localStorage.getItem("gb-speed") || 1);
  var volume = Number(localStorage.getItem("gb-volume") || 100);
  var palette = localStorage.getItem("gb-palette") || "none";
  var remapTarget = null;

  var KEYMAP = {
    ArrowRight: 1, ArrowLeft: 2, ArrowUp: 4, ArrowDown: 8,
    KeyZ: 16, KeyX: 32, ShiftLeft: 64, ShiftRight: 64,
    Enter: 128
  };
  var keyNames = {ArrowRight: "RIGHT", ArrowLeft: "LEFT", ArrowUp: "UP", ArrowDown: "DOWN",
    KeyZ: "Z", KeyX: "X", ShiftLeft: "SHIFT", ShiftRight: "SHIFT", Enter: "ENTER"};

  var audioCtx = null;
  var gainNode = null;

  function status(text) { statusEl.textContent = text; }

  function ensureAudio() {
    if (audioCtx)
      return;
    var Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx)
      return;
    try {
      audioCtx = new Ctx({ sampleRate: 48000 });
    } catch (e) {
      audioCtx = null;
      return;
    }
    var pump = audioCtx.createScriptProcessor(4096, 0, 2);
    pump.onaudioprocess = function (event) {
      var left = event.outputBuffer.getChannelData(0);
      var right = event.outputBuffer.getChannelData(1);
      var avail = Module.ccall("wasm_audio_available", "number", [], []);
      var take = Math.min(avail, left.length);
      if (take > 0) {
        var ptr = Module.ccall("wasm_audio_ptr", "number", [], []);
        var heap = Module.HEAP16;
        for (var i = 0; i < take; i++) {
          left[i] = heap[(ptr >> 1) + i * 2] / 32768;
          right[i] = heap[(ptr >> 1) + i * 2 + 1] / 32768;
        }
        Module.ccall("wasm_audio_consume", null, ["number"], [take]);
      }
      for (var j = take; j < left.length; j++) {
        left[j] = 0;
        right[j] = 0;
      }
    };
    gainNode = audioCtx.createGain();
    gainNode.gain.value = volume / 100;
    pump.connect(gainNode);
    gainNode.connect(audioCtx.destination);
  }

  function copyFrame(ptr) {
    var heap = Module.HEAPU32;
    var pixels = image.data;
    for (var i = 0; i < 160 * 144; i++) {
      var p = heap[(ptr >> 2) + i];
      var o = i * 4;
      pixels[o] = (p >> 16) & 255;
      pixels[o + 1] = (p >> 8) & 255;
      pixels[o + 2] = p & 255;
      pixels[o + 3] = 255;
    }
    ctx.putImageData(image, 0, 0);
  }

  function frame() {
    if (!running)
      return;
    for (var i = 0; i < speed; i++)
      Module.ccall("wasm_run_frame", null, [], []);
    copyFrame(Module.ccall("wasm_framebuffer", "number", [], []));
    requestAnimationFrame(frame);
  }

  function persistBattery() {
    var size = Module.ccall("wasm_save_size", "number", [], []);
    if (!size)
      return;
    var ptr = Module._malloc(size);
    var written = Module.ccall("wasm_save", "number", ["number"], [ptr]);
    var bytes = new Uint8Array(Module.HEAPU8.buffer, ptr, written);
    var binary = "";
    for (var i = 0; i < bytes.length; i++)
      binary += String.fromCharCode(bytes[i]);
    try {
      localStorage.setItem("gb-battery", btoa(binary));
    } catch (e) { /* storage full or unavailable; ignore */ }
    Module._free(ptr);
  }

  function restoreBattery() {
    var raw = null;
    try {
      raw = localStorage.getItem("gb-battery");
    } catch (e) { return; }
    if (!raw)
      return;
    var binary = atob(raw);
    var ptr = Module._malloc(binary.length);
    Module.HEAPU8.set(
      Array.prototype.map.call(binary, function (c) { return c.charCodeAt(0); }), ptr);
    Module.ccall("wasm_load_save", "number", ["number", "number"],
      [ptr, binary.length]);
    Module._free(ptr);
  }

  function loadRomBytes(bytes, name) {
    var ptr = Module._malloc(bytes.length);
    Module.HEAPU8.set(bytes, ptr);
    Module.ccall("wasm_boot", "number", [], []);
    var rc = Module.ccall("wasm_load", "number", ["number", "number"],
      [ptr, bytes.length]);
    Module._free(ptr);
    if (rc !== 0) { status("Could not load ROM."); return; }
    restoreBattery();
    booted = running = true;
    ensureAudio();
    if (audioCtx && audioCtx.state === "suspended") audioCtx.resume();
    status("Running: " + name);
    requestAnimationFrame(frame);
  }

  document.getElementById("rom").addEventListener("change", function (event) {
    var file = event.target.files[0];
    if (!file)
      return;
    var reader = new FileReader();
    reader.onload = function () {
      loadRomBytes(new Uint8Array(reader.result), file.name);
    };
    reader.readAsArrayBuffer(file);
  });

  document.getElementById("settings-toggle").addEventListener("click", function () {
    var panel = document.getElementById("settings");
    var open = panel.classList.toggle("open");
    panel.setAttribute("aria-hidden", open ? "false" : "true");
    this.textContent = open ? "Close Settings" : "Settings";
  });

  document.getElementById("pause").addEventListener("click", function () {
    if (!booted)
      return;
    running = !running;
    this.textContent = running ? "Pause" : "Resume";
    if (running)
      requestAnimationFrame(frame);
    else
      persistBattery();
  });

  document.getElementById("reset").addEventListener("click", function () {
    if (!booted)
      return;
    Module.ccall("wasm_reset", null, [], []);
  });

  document.getElementById("savestate").addEventListener("click", function () {
    if (!booted)
      return;
    var size = Module.ccall("wasm_state_size", "number", [], []);
    var ptr = Module._malloc(size);
    var written = Module.ccall("wasm_state_save", "number", ["number"], [ptr]);
    var bytes = new Uint8Array(Module.HEAPU8.buffer, ptr, written);
    var binary = "";
    for (var i = 0; i < bytes.length; i++)
      binary += String.fromCharCode(bytes[i]);
    try {
      localStorage.setItem("gb-state", btoa(binary));
      status("State saved.");
    } catch (e) {
      status("State save failed.");
    }
    Module._free(ptr);
  });

  document.getElementById("loadstate").addEventListener("click", function () {
    if (!booted)
      return;
    var raw = null;
    try {
      raw = localStorage.getItem("gb-state");
    } catch (e) { /* ignore */ }
    if (!raw) {
      status("No saved state.");
      return;
    }
    var binary = atob(raw);
    var ptr = Module._malloc(binary.length);
    Module.HEAPU8.set(
      Array.prototype.map.call(binary, function (c) { return c.charCodeAt(0); }), ptr);
    var rc = Module.ccall("wasm_state_load", "number", ["number", "number"],
      [ptr, binary.length]);
    Module._free(ptr);
    status(rc === 0 ? "State loaded." : "State load failed.");
  });

  document.getElementById("volume").oninput = function () {
    volume = Number(this.value);
    localStorage.setItem("gb-volume", volume);
    document.getElementById("volume-value").textContent = volume + "%";
    if (gainNode) gainNode.gain.value = volume / 100;
  };
  document.getElementById("palette").onchange = function () {
    palette = this.value;
    localStorage.setItem("gb-palette", palette);
    canvas.style.filter = palette === "green" ? "sepia(.35) saturate(.7) hue-rotate(55deg)" :
      palette === "sepia" ? "sepia(.8) saturate(.8)" : "none";
  };
  document.getElementById("speed").onchange = function () {
    speed = Number(this.value);
    localStorage.setItem("gb-speed", speed);
  };
  function renderKeymap() {
    var root = document.getElementById("keymap");
    root.textContent = "";
    Object.keys(KEYMAP).forEach(function (code) {
      var button = document.createElement("button");
      button.textContent = keyNames[code] || code;
      button.onclick = function () { remapTarget = code; button.textContent = "PRESS KEY"; };
      root.appendChild(button);
    });
  }
  document.getElementById("volume").value = volume;
  document.getElementById("volume-value").textContent = volume + "%";
  document.getElementById("palette").value = palette;
  document.getElementById("speed").value = String(speed);
  document.getElementById("palette").dispatchEvent(new Event("change"));
  renderKeymap();

  window.addEventListener("keydown", function (event) {
    if (remapTarget) {
      var old = remapTarget;
      KEYMAP[event.code] = KEYMAP[old];
      keyNames[event.code] = keyNames[old] || event.code;
      if (event.code !== old) delete KEYMAP[old];
      remapTarget = null;
      renderKeymap();
      event.preventDefault();
      return;
    }
    var bit = KEYMAP[event.code];
    if (bit === undefined || !booted)
      return;
    buttons |= bit;
    Module.ccall("wasm_set_input", null, ["number"], [buttons]);
    event.preventDefault();
  });

  window.addEventListener("keyup", function (event) {
    var bit = KEYMAP[event.code];
    if (bit === undefined || !booted)
      return;
    buttons &= ~bit;
    Module.ccall("wasm_set_input", null, ["number"], [buttons]);
  });

  window.addEventListener("pagehide", persistBattery);
  setInterval(function () { if (booted && running) persistBattery(); }, 10000);
}());
