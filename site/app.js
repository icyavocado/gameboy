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
  var storedVolume = localStorage.getItem("gb-volume");
  var volume = storedVolume === null ? 0 : Number(storedVolume);
  var palette = localStorage.getItem("gb-palette") || "none";
  var saveSlot = localStorage.getItem("gb-save-slot") || "A";
  var autoSave = Number(localStorage.getItem("gb-auto-save") || 10);
  var showPad = localStorage.getItem("gb-show-pad") === "1";
  var currentRomKey = "default";
  var autoSaveTimer = null;
  var remapTarget = null;

  var KEYMAP = {
    ArrowRight: 1, ArrowLeft: 2, ArrowUp: 4, ArrowDown: 8,
    KeyZ: 16, KeyX: 32, ShiftLeft: 64, ShiftRight: 64,
    Enter: 128
  };
  var keyNames = {ArrowRight: "RIGHT", ArrowLeft: "LEFT", ArrowUp: "UP", ArrowDown: "DOWN",
    KeyZ: "Z", KeyX: "X", ShiftLeft: "SHIFT", ShiftRight: "SHIFT", Enter: "ENTER"};

  function setButton(bit, pressed, element) {
    if (pressed) buttons |= bit;
    else buttons &= ~bit;
    element.classList.toggle("held", pressed);
    if (booted)
      Module.ccall("wasm_set_input", null, ["number"], [buttons]);
  }

  var audioCtx = null;
  var gainNode = null;
  var audioStarted = false;
  var nextAudioTime = 0;
  var AUDIO_PREBUFFER = 4800;
  var AUDIO_CHUNK = 1024;
  var AUDIO_LOOKAHEAD = 0.25;
  /* Audio-driven pacing: keep this much audio queued (WASM ring + Web Audio
     scheduled) and run more/fewer emulator frames per tick to hold it. */
  var AUDIO_TARGET_MIN = 0.08;
  var AUDIO_TARGET_MAX = 0.20;
  var MAX_FRAMES_PER_TICK = 4;

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
    gainNode = audioCtx.createGain();
    gainNode.gain.value = volume / 100;
  }

  function scheduleAudio() {
    if (!audioCtx)
      return;
    var available = Module.ccall("wasm_audio_available", "number", [], []);
    if (!audioStarted && available < AUDIO_PREBUFFER)
      return;
    if (!audioStarted) {
      nextAudioTime = audioCtx.currentTime + 0.08;
      gainNode.connect(audioCtx.destination);
      audioStarted = true;
    }
    if (audioCtx.state === "suspended")
      audioCtx.resume();
    if (nextAudioTime < audioCtx.currentTime)
      nextAudioTime = audioCtx.currentTime + 0.02;
    while (available >= AUDIO_CHUNK &&
           nextAudioTime < audioCtx.currentTime + AUDIO_LOOKAHEAD) {
      var ptr = Module.ccall("wasm_audio_ptr", "number", [], []);
      var buffer = audioCtx.createBuffer(2, AUDIO_CHUNK, 48000);
      var left = buffer.getChannelData(0);
      var right = buffer.getChannelData(1);
      var heap = Module.HEAP16;
      for (var i = 0; i < AUDIO_CHUNK; i++) {
        left[i] = heap[(ptr >> 1) + i * 2] / 32768;
        right[i] = heap[(ptr >> 1) + i * 2 + 1] / 32768;
      }
      var source = audioCtx.createBufferSource();
      source.buffer = buffer;
      source.connect(gainNode);
      source.start(nextAudioTime);
      nextAudioTime += AUDIO_CHUNK / 48000;
      Module.ccall("wasm_audio_consume", null, ["number"], [AUDIO_CHUNK]);
      available -= AUDIO_CHUNK;
    }
  }

  function unlockAudio() {
    if (!booted)
      return;
    ensureAudio();
    if (audioCtx && audioCtx.state === "suspended")
      audioCtx.resume();
    scheduleAudio();
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

  function queuedAudioSeconds() {
    var ring = Module.ccall("wasm_audio_available", "number", [], []) / 48000;
    var scheduled = audioStarted ? Math.max(0, nextAudioTime - audioCtx.currentTime) : 0;
    return ring + scheduled;
  }

  function frame() {
    if (!running)
      return;
    var runs = speed;
    if (speed === 1 && audioStarted && audioCtx) {
      /* Hold the audio queue in [MIN, MAX] regardless of display refresh rate. */
      var queued = queuedAudioSeconds();
      if (queued > AUDIO_TARGET_MAX)
        runs = 0;
      else if (queued < AUDIO_TARGET_MIN)
        runs = MAX_FRAMES_PER_TICK;
    }
    for (var i = 0; i < runs; i++)
      Module.ccall("wasm_run_frame", null, [], []);
    if (speed > 1) {
      /* Fast-forward: keep only the freshest audio so the queue never wraps. */
      var excess = Module.ccall("wasm_audio_available", "number", [], []) - AUDIO_PREBUFFER;
      if (excess > 0)
        Module.ccall("wasm_audio_consume", null, ["number"], [excess]);
    }
    scheduleAudio();
    copyFrame(Module.ccall("wasm_framebuffer", "number", [], []));
    requestAnimationFrame(frame);
  }

  function batteryKey() {
    return "gb-battery-" + encodeURIComponent(currentRomKey) + "-" + saveSlot;
  }

  function batteryData() {
    var size = Module.ccall("wasm_save_size", "number", [], []);
    if (!size)
      return null;
    var ptr = Module._malloc(size);
    var written = Module.ccall("wasm_save", "number", ["number"], [ptr]);
    var bytes = new Uint8Array(Module.HEAPU8.buffer, ptr, written).slice();
    var binary = "";
    for (var i = 0; i < bytes.length; i++)
      binary += String.fromCharCode(bytes[i]);
    Module._free(ptr);
    return btoa(binary);
  }

  function persistBattery() {
    var data = batteryData();
    if (!data)
      return;
    try {
      localStorage.setItem(batteryKey(), data);
    } catch (e) { /* storage full or unavailable; ignore */ }
  }

  function restoreBattery() {
    var raw = null;
    try {
      raw = localStorage.getItem(batteryKey());
      if (!raw && saveSlot === "A")
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

  function loadBatteryData(raw) {
    if (!raw) return -1;
    var binary = atob(raw);
    var ptr = Module._malloc(binary.length);
    Module.HEAPU8.set(Array.prototype.map.call(binary, function (c) {
      return c.charCodeAt(0);
    }), ptr);
    var rc = Module.ccall("wasm_load_save", "number", ["number", "number"],
      [ptr, binary.length]);
    Module._free(ptr);
    return rc;
  }

  function scheduleAutoSave() {
    if (autoSaveTimer) clearInterval(autoSaveTimer);
    autoSaveTimer = autoSave ? setInterval(function () {
      if (booted) persistBattery();
    }, autoSave * 1000) : null;
  }

  function stateData() {
    var size = Module.ccall("wasm_state_size", "number", [], []);
    if (!size) return null;
    var ptr = Module._malloc(size);
    var written = Module.ccall("wasm_state_save", "number", ["number"], [ptr]);
    var bytes = new Uint8Array(Module.HEAPU8.buffer, ptr, written).slice();
    Module._free(ptr);
    return bytes;
  }

  function downloadBytes(bytes, filename, type) {
    var link = document.createElement("a");
    link.href = URL.createObjectURL(new Blob([bytes], {type: type}));
    link.download = filename;
    link.click();
    URL.revokeObjectURL(link.href);
  }

  function loadRomBytes(bytes, name) {
    var ptr = Module._malloc(bytes.length);
    Module.HEAPU8.set(bytes, ptr);
    Module.ccall("wasm_boot", "number", [], []);
    var rc = Module.ccall("wasm_load", "number", ["number", "number"],
      [ptr, bytes.length]);
    Module._free(ptr);
    if (rc !== 0) { status("Could not load ROM."); return; }
    currentRomKey = name.replace(/[^a-z0-9._-]/gi, "_");
    restoreBattery();
    booted = running = true;
    ensureAudio();
    unlockAudio();
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
  document.getElementById("download-state").onclick = function () {
    if (!booted) return;
    var bytes = stateData();
    if (!bytes) { status("Could not create state."); return; }
    downloadBytes(bytes, currentRomKey + ".state", "application/octet-stream");
    status("State downloaded.");
  };
  document.getElementById("import-state").onclick = function () {
    if (booted) document.getElementById("state-file").click();
  };
  document.getElementById("state-file").onchange = function (event) {
    var file = event.target.files[0];
    if (!file) return;
    var reader = new FileReader();
    reader.onload = function () {
      var bytes = new Uint8Array(reader.result);
      var ptr = Module._malloc(bytes.length);
      Module.HEAPU8.set(bytes, ptr);
      var rc = Module.ccall("wasm_state_load", "number", ["number", "number"],
        [ptr, bytes.length]);
      Module._free(ptr);
      status(rc === 0 ? "State imported." : "State file rejected.");
    };
    reader.readAsArrayBuffer(file);
  };

  document.getElementById("save-slot").onchange = function () {
    saveSlot = this.value;
    localStorage.setItem("gb-save-slot", saveSlot);
  };
  document.getElementById("auto-save").onchange = function () {
    autoSave = Number(this.value);
    localStorage.setItem("gb-auto-save", autoSave);
    scheduleAutoSave();
  };
  document.getElementById("save-slot-now").onclick = function () {
    if (!booted) return;
    persistBattery();
    status("Save slot " + saveSlot + " saved.");
  };
  document.getElementById("load-slot").onclick = function () {
    if (!booted) return;
    var raw = localStorage.getItem(batteryKey());
    if (loadBatteryData(raw) === 0)
      status("Save slot " + saveSlot + " loaded.");
    else
      status("No save in slot " + saveSlot + ".");
  };
  document.getElementById("download-save").onclick = function () {
    if (!booted) return;
    var data = batteryData();
    if (!data) { status("This ROM has no battery save data."); return; }
    var binary = atob(data);
    var bytes = new Uint8Array(binary.length);
    for (var i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
    var link = document.createElement("a");
    link.href = URL.createObjectURL(new Blob([bytes], {type: "application/octet-stream"}));
    link.download = currentRomKey + ".sav";
    link.click();
    URL.revokeObjectURL(link.href);
    status("Save downloaded.");
  };
  document.getElementById("import-save").onclick = function () {
    if (booted) document.getElementById("save-file").click();
  };
  document.getElementById("save-file").onchange = function (event) {
    var file = event.target.files[0];
    if (!file) return;
    var reader = new FileReader();
    reader.onload = function () {
      var bytes = new Uint8Array(reader.result);
      var ptr = Module._malloc(bytes.length);
      Module.HEAPU8.set(bytes, ptr);
      var rc = Module.ccall("wasm_load_save", "number", ["number", "number"],
        [ptr, bytes.length]);
      Module._free(ptr);
      if (rc === 0) {
        persistBattery();
        status("Save imported into slot " + saveSlot + ".");
      } else status("Save file rejected.");
    };
    reader.readAsArrayBuffer(file);
  };

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

  document.querySelectorAll(".pad-button").forEach(function (button) {
    var bit = Number(button.dataset.bit);
    button.addEventListener("pointerdown", function (event) {
      event.preventDefault();
      button.setPointerCapture(event.pointerId);
      setButton(bit, true, button);
    });
    button.addEventListener("pointerup", function (event) {
      event.preventDefault();
      setButton(bit, false, button);
    });
    button.addEventListener("pointercancel", function () {
      setButton(bit, false, button);
    });
  });
  document.getElementById("volume").value = volume;
  document.getElementById("volume-value").textContent = volume + "%";
  document.getElementById("palette").value = palette;
  document.getElementById("speed").value = String(speed);
  document.getElementById("show-pad").checked = showPad;
  document.getElementById("gamepad").classList.toggle("visible", showPad);
  document.getElementById("show-pad").onchange = function () {
    showPad = this.checked;
    localStorage.setItem("gb-show-pad", showPad ? "1" : "0");
    document.getElementById("gamepad").classList.toggle("visible", showPad);
  };
  document.getElementById("save-slot").value = saveSlot;
  document.getElementById("auto-save").value = String(autoSave);
  document.getElementById("palette").dispatchEvent(new Event("change"));
  renderKeymap();
  scheduleAutoSave();

  window.addEventListener("keydown", function (event) {
    unlockAudio();
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
  window.addEventListener("pointerdown", unlockAudio);

  window.addEventListener("pagehide", persistBattery);
}());
