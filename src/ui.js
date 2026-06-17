/*
 * Mini-JV Module UI - Hierarchy-Driven Version
 *
 * Reads ui_hierarchy from the plugin and renders menus based on it.
 * Same UI structure in both standalone and shadow mode.
 */

import {
    MoveMainKnob, MoveMainButton,
    MoveLeft, MoveRight, MoveUp, MoveDown,
    MoveShift, MoveMenu, MoveBack,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    MovePads
} from '/data/UserData/schwung/shared/constants.mjs';

import { isCapacitiveTouchMessage, decodeDelta } from '/data/UserData/schwung/shared/input_filter.mjs';

/* === Constants === */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

const CC_JOG = MoveMainKnob;
const CC_JOG_CLICK = MoveMainButton;
const CC_LEFT = MoveLeft;
const CC_RIGHT = MoveRight;
const CC_UP = MoveUp;
const CC_DOWN = MoveDown;
const CC_SHIFT = MoveShift;
const CC_MENU = MoveMenu;
const CC_BACK = MoveBack;

const ENCODER_CCS = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];

/* === State === */
let hierarchy = null;       // Parsed ui_hierarchy
let chainParams = null;     // Parsed chain_params (metadata)
let chainParamsMap = {};    // key -> param metadata

let currentMode = 'patch';  // 'patch' or 'performance'
let levelStack = [];        // Stack of level names for navigation
let currentLevel = null;    // Current level object
let menuIndex = 0;          // Selected menu item index
let menuScroll = 0;         // Scroll offset for long menus

// For child selectors (tones/parts)
let selectedChildIndex = 0;

// For list browsers (presets, performances)
let listIndex = 0;
let listCount = 0;
let listName = '';

// For items selectors (expansions, user patches)
let itemsList = [];

// Editing state
let editingParam = null;    // Currently editing parameter key
let editingValue = 0;       // Current value being edited

// Knob overlay state
let knobOverlayParam = null;   // Param key being shown in overlay
let knobOverlayValue = 0;      // Value at time of last knob turn
let knobOverlayTick = 0;       // Tick when overlay was last updated
const KNOB_OVERLAY_TICKS = 60; // ~2 seconds at 30fps

let shiftHeld = false;
let needsRedraw = true;
let loadingComplete = false;
let tickCount = 0;

/* === Initialization === */
function init() {
    console.log("Mini-JV Hierarchy UI: init");
    loadHierarchy();
}

function loadHierarchy() {
    const hierJson = host_module_get_param('ui_hierarchy');
    const paramsJson = host_module_get_param('chain_params');

    if (hierJson) {
        try {
            hierarchy = JSON.parse(hierJson);
            console.log("Hierarchy loaded: modes=" + JSON.stringify(hierarchy.modes));
        } catch (e) {
            console.log("Error parsing hierarchy: " + e);
        }
    }

    if (paramsJson) {
        try {
            chainParams = JSON.parse(paramsJson);
            // Build lookup map
            chainParamsMap = {};
            for (const p of chainParams) {
                chainParamsMap[p.key] = p;
            }
            console.log("Chain params loaded: " + chainParams.length + " params");
        } catch (e) {
            console.log("Error parsing chain_params: " + e);
        }
    }

    if (hierarchy) {
        // Start in first mode's root level
        currentMode = hierarchy.modes ? hierarchy.modes[0] : 'patch';
        navigateToLevel(currentMode);
        loadingComplete = true;
    }
}

/* === Level Navigation === */
function navigateToLevel(levelName) {
    if (!hierarchy || !hierarchy.levels[levelName]) {
        console.log("Level not found: " + levelName);
        return;
    }

    currentLevel = hierarchy.levels[levelName];
    levelStack.push(levelName);
    menuIndex = 0;
    menuScroll = 0;
    selectedChildIndex = 0;
    editingParam = null;

    // If this is a list browser, load list state
    if (currentLevel.list_param) {
        listIndex = parseInt(host_module_get_param(currentLevel.list_param) || '0');
        listCount = parseInt(host_module_get_param(currentLevel.count_param) || '0');
        listName = host_module_get_param(currentLevel.name_param) || '';
    }

    // If this is an items selector, load items
    if (currentLevel.items_param) {
        const itemsJson = host_module_get_param(currentLevel.items_param);
        if (itemsJson) {
            try {
                itemsList = JSON.parse(itemsJson);
            } catch (e) {
                itemsList = [];
            }
        } else {
            itemsList = [];
        }
    }

    needsRedraw = true;
    announceView(currentLevel.label || levelName);
    console.log("Navigated to level: " + levelName);
}

function navigateBack() {
    if (levelStack.length > 1) {
        levelStack.pop();
        const prevLevel = levelStack[levelStack.length - 1];
        levelStack.pop(); // Will be pushed again by navigateToLevel
        navigateToLevel(prevLevel);
    }
}

function navigateToChildren() {
    if (currentLevel && currentLevel.children) {
        navigateToLevel(currentLevel.children);
    }
}

/* === Mode Switching === */
function switchMode(newMode) {
    if (newMode !== currentMode && hierarchy.modes && hierarchy.modes.includes(newMode)) {
        currentMode = newMode;
        host_module_set_param('mode', currentMode === 'performance' ? '1' : '0');
        levelStack = [];
        navigateToLevel(currentMode);
    }
}

/* === Screen-reader announcements ===
 * The host has no dedicated TTS channel today, so these route through the same
 * console.log the rest of the UI uses. They give a single, named hook point
 * (matching the platform convention) that a screen-reader layer can later
 * intercept without touching call sites — no new subsystem is introduced. */
function announceView(label) {
    console.log("[view] " + label);
}
function announceMenuItem(label, value) {
    console.log("[item] " + label + (value !== undefined && value !== '' ? ": " + value : ""));
}

/* === Parameter Handling === */
function getParamMeta(key) {
    return chainParamsMap[key] || { name: key, type: 'int', min: 0, max: 127 };
}

function getFullParamKey(key) {
    // Handle child prefix (e.g., part_selector -> sram_part_0_partlevel).
    // NOTE: per-tone editing now uses EXPLICIT per-tone levels whose param keys
    // are already fully qualified (nvram_tone_<n>_<param>), matching what the
    // platform's shadow_ui.js renderer requires in a chain slot. No tone_section
    // key rewriting is done here.
    if (currentLevel.child_prefix) {
        return currentLevel.child_prefix + selectedChildIndex + '_' + key;
    }
    return key;
}

function getParamValue(key) {
    const fullKey = getFullParamKey(key);
    const val = host_module_get_param(fullKey);
    return parseInt(val || '0');
}

function setParamValue(key, value) {
    const fullKey = getFullParamKey(key);
    const meta = getParamMeta(key);

    // Clamp to valid range
    if (meta.min !== undefined) value = Math.max(meta.min, value);
    if (meta.max !== undefined) value = Math.min(meta.max, value);

    host_module_set_param(fullKey, String(value));
    needsRedraw = true;
}

/* === Input Handling === */
function handleCC(cc, value) {
    if (cc === CC_SHIFT) {
        shiftHeld = (value > 0);
        return;
    }

    if (value === 0) return; // Button release

    // Mode switching with Shift + Left/Right
    if (shiftHeld && hierarchy.modes && hierarchy.modes.length > 1) {
        if (cc === CC_LEFT) {
            const idx = hierarchy.modes.indexOf(currentMode);
            if (idx > 0) switchMode(hierarchy.modes[idx - 1]);
            return;
        }
        if (cc === CC_RIGHT) {
            const idx = hierarchy.modes.indexOf(currentMode);
            if (idx < hierarchy.modes.length - 1) switchMode(hierarchy.modes[idx + 1]);
            return;
        }
    }

    // Back button
    if (cc === CC_BACK || cc === CC_MENU) {
        if (editingParam) {
            editingParam = null;
            needsRedraw = true;
        } else {
            navigateBack();
        }
        return;
    }

    // If editing a parameter
    if (editingParam) {
        handleEditingInput(cc, value);
        return;
    }

    // List browser navigation (presets, performances)
    if (currentLevel.list_param) {
        handleListInput(cc, value);
        return;
    }

    // Items selector navigation (expansions, user patches)
    if (currentLevel.items_param) {
        handleItemsInput(cc, value);
        return;
    }

    // Child selector navigation (tones, parts)
    if (currentLevel.child_count) {
        handleChildSelectorInput(cc, value);
        return;
    }

    // Menu navigation
    handleMenuInput(cc, value);
}

function handleListInput(cc, value) {
    const delta = (cc === CC_JOG) ? decodeDelta(value) : 0;

    if (cc === CC_LEFT || delta < 0) {
        listIndex = Math.max(0, listIndex - 1);
        host_module_set_param(currentLevel.list_param, String(listIndex));
        listName = host_module_get_param(currentLevel.name_param) || '';
        needsRedraw = true;
    } else if (cc === CC_RIGHT || delta > 0) {
        listIndex = Math.min(listCount - 1, listIndex + 1);
        host_module_set_param(currentLevel.list_param, String(listIndex));
        listName = host_module_get_param(currentLevel.name_param) || '';
        needsRedraw = true;
    } else if (cc === CC_JOG_CLICK || cc === CC_DOWN) {
        // Enter children or main menu
        navigateToChildren();
    } else if (cc === CC_UP) {
        navigateBack();
    }
}

function handleItemsInput(cc, value) {
    const delta = (cc === CC_JOG) ? decodeDelta(value) : 0;

    if (cc === CC_UP || delta < 0) {
        menuIndex = Math.max(0, menuIndex - 1);
        needsRedraw = true;
    } else if (cc === CC_DOWN || delta > 0) {
        menuIndex = Math.min(itemsList.length - 1, menuIndex + 1);
        needsRedraw = true;
    } else if (cc === CC_JOG_CLICK) {
        // Select item
        if (itemsList[menuIndex]) {
            const item = itemsList[menuIndex];
            const selectValue = item.index !== undefined ? item.index : menuIndex;
            host_module_set_param(currentLevel.select_param, String(selectValue));

            // Check if this level specifies where to navigate after selection
            if (currentLevel.navigate_to) {
                // Clear stack and navigate to specified level
                levelStack = [];
                navigateToLevel(currentLevel.navigate_to);
            } else {
                navigateBack();
            }
        }
    }
}

function handleChildSelectorInput(cc, value) {
    const delta = (cc === CC_JOG) ? decodeDelta(value) : 0;
    const params = currentLevel.params || [];

    if (cc === CC_LEFT) {
        selectedChildIndex = Math.max(0, selectedChildIndex - 1);
        needsRedraw = true;
    } else if (cc === CC_RIGHT) {
        selectedChildIndex = Math.min(currentLevel.child_count - 1, selectedChildIndex + 1);
        needsRedraw = true;
    } else if (cc === CC_UP || delta < 0) {
        menuIndex = Math.max(0, menuIndex - 1);
        needsRedraw = true;
    } else if (cc === CC_DOWN || delta > 0) {
        menuIndex = Math.min(params.length - 1, menuIndex + 1);
        needsRedraw = true;
    } else if (cc === CC_JOG_CLICK) {
        const item = params[menuIndex];
        // Navigation item (e.g. a tone or section page): descend into it. Target
        // levels are explicit and carry fully-qualified keys, so no per-level
        // tone state needs to be persisted.
        if (item && typeof item === 'object' && item.level) {
            announceView(item.label || item.level);
            navigateToLevel(item.level);
            return;
        }
        // Otherwise start editing the selected param for the current tone.
        const paramKey = typeof item === 'string' ? item : (item && item.key);
        if (paramKey) {
            editingParam = paramKey;
            editingValue = getParamValue(paramKey);
            announceMenuItem(getParamMeta(paramKey).name || paramKey,
                             formatValue(editingValue, getParamMeta(paramKey)));
            needsRedraw = true;
        }
    }
}

function handleMenuInput(cc, value) {
    const delta = (cc === CC_JOG) ? decodeDelta(value) : 0;
    const params = currentLevel.params || [];

    if (cc === CC_UP || delta < 0) {
        menuIndex = Math.max(0, menuIndex - 1);
        needsRedraw = true;
    } else if (cc === CC_DOWN || delta > 0) {
        menuIndex = Math.min(params.length - 1, menuIndex + 1);
        needsRedraw = true;
    } else if (cc === CC_JOG_CLICK) {
        const item = params[menuIndex];
        if (item) {
            if (typeof item === 'string') {
                // Simple param key - start editing
                editingParam = item;
                editingValue = getParamValue(item);
                announceMenuItem(getParamMeta(item).name || item,
                                 formatValue(editingValue, getParamMeta(item)));
            } else if (item.level) {
                // Navigation to sublevel
                navigateToLevel(item.level);
            } else if (item.key) {
                // Param with label - start editing
                editingParam = item.key;
                editingValue = getParamValue(item.key);
                announceMenuItem(item.label || getParamMeta(item.key).name || item.key,
                                 formatValue(editingValue, getParamMeta(item.key)));
            }
            needsRedraw = true;
        }
    }
}

function handleEditingInput(cc, value) {
    const delta = (cc === CC_JOG) ? decodeDelta(value) : 0;
    const meta = getParamMeta(editingParam);

    if (delta !== 0) {
        const step = shiftHeld ? 10 : 1;
        editingValue += delta * step;
        if (meta.min !== undefined) editingValue = Math.max(meta.min, editingValue);
        if (meta.max !== undefined) editingValue = Math.min(meta.max, editingValue);
        setParamValue(editingParam, editingValue);
    } else if (cc === CC_JOG_CLICK || cc === CC_BACK) {
        editingParam = null;
        needsRedraw = true;
    } else if (cc === CC_LEFT) {
        editingValue--;
        if (meta.min !== undefined) editingValue = Math.max(meta.min, editingValue);
        setParamValue(editingParam, editingValue);
    } else if (cc === CC_RIGHT) {
        editingValue++;
        if (meta.max !== undefined) editingValue = Math.min(meta.max, editingValue);
        setParamValue(editingParam, editingValue);
    }
}

function handleKnob(knobIndex, delta) {
    if (!currentLevel || !currentLevel.knobs) return;

    const knobs = currentLevel.knobs;
    if (knobIndex >= knobs.length) return;

    const paramKey = knobs[knobIndex];
    const meta = getParamMeta(paramKey);
    let value = getParamValue(paramKey);

    const step = shiftHeld ? 10 : 1;
    value += delta * step;

    setParamValue(paramKey, value);

    // Update knob overlay state
    knobOverlayParam = paramKey;
    knobOverlayValue = getParamValue(paramKey); // read back clamped value
    knobOverlayTick = tickCount;
    needsRedraw = true;
}

/* === Drawing === */
function draw() {
    display.clear();

    if (!loadingComplete || !hierarchy) {
        drawLoading();
        display.flush();
        return;
    }

    if (currentLevel.list_param) {
        drawListBrowser();
    } else if (currentLevel.items_param) {
        drawItemsSelector();
    } else if (currentLevel.child_count) {
        drawChildSelector();
    } else {
        drawMenu();
    }

    // Draw knob overlay on top if active
    if (knobOverlayParam && (tickCount - knobOverlayTick) < KNOB_OVERLAY_TICKS) {
        drawKnobOverlay();
    } else if (knobOverlayParam) {
        // Overlay expired — clear state and force a clean redraw next tick
        knobOverlayParam = null;
        needsRedraw = true;
    }

    display.flush();
}

function drawLoading() {
    const status = host_module_get_param('loading_status') || 'Loading...';
    display.drawText(4, 28, status, 1);
}

function drawKnobLabels(y) {
    if (!currentLevel || !currentLevel.knob_labels) return;
    const labels = currentLevel.knob_labels;
    const spacing = SCREEN_WIDTH / 8;
    for (let i = 0; i < labels.length && i < 8; i++) {
        const lbl = labels[i];
        const x = Math.floor(i * spacing + (spacing - lbl.length * 6) / 2);
        display.drawText(x, y, lbl, 1);
    }
}

function drawListBrowser() {
    // Header with mode
    const modeLabel = currentMode === 'performance' ? 'PERF' : 'PATCH';
    display.drawText(0, 0, modeLabel, 1);

    // Preset number
    const numStr = String(listIndex + 1) + '/' + String(listCount);
    display.drawText(SCREEN_WIDTH - numStr.length * 6, 0, numStr, 1);

    // Preset name (large, centered)
    const name = listName || '---';
    const nameX = Math.max(0, (SCREEN_WIDTH - name.length * 6) / 2);
    display.drawText(nameX, 24, name, 1);

    // Bank name if available
    const bankName = host_module_get_param('bank_name') || '';
    if (bankName) {
        const bankX = Math.max(0, (SCREEN_WIDTH - bankName.length * 6) / 2);
        display.drawText(bankX, 12, bankName, 1);
    }

    // Knob labels
    drawKnobLabels(44);

    // Footer hint
    display.drawText(0, 56, 'Jog:Browse  Click:Menu', 1);
}

function drawItemsSelector() {
    // Header
    const label = currentLevel.label || 'Select';
    display.drawText(0, 0, label, 1);
    display.drawLine(0, 10, SCREEN_WIDTH, 10, 1);

    // Items list
    const visibleItems = 5;
    const startIdx = Math.max(0, menuIndex - 2);

    for (let i = 0; i < visibleItems && startIdx + i < itemsList.length; i++) {
        const idx = startIdx + i;
        const item = itemsList[idx];
        const y = 14 + i * 10;
        const label = item.label || item.name || `Item ${idx + 1}`;

        if (idx === menuIndex) {
            display.fillRect(0, y - 1, SCREEN_WIDTH, 10, 1);
            display.drawText(4, y, label, 0);
        } else {
            display.drawText(4, y, label, 1);
        }
    }
}

function drawChildSelector() {
    const t0 = Date.now();
    // Header with child tabs
    const label = currentLevel.child_label || 'Item';
    for (let i = 0; i < currentLevel.child_count; i++) {
        const x = i * 32;
        const tabLabel = label + ' ' + (i + 1);
        if (i === selectedChildIndex) {
            display.fillRect(x, 0, 30, 10, 1);
            display.drawText(x + 2, 1, tabLabel, 0);
        } else {
            display.drawText(x + 2, 1, tabLabel, 1);
        }
    }

    display.drawLine(0, 11, SCREEN_WIDTH, 11, 1);

    // Params list
    const params = currentLevel.params || [];
    const visibleItems = 5;
    const startIdx = Math.max(0, menuIndex - 2);

    let getParamTime = 0;
    let drawTime = 0;

    for (let i = 0; i < visibleItems && startIdx + i < params.length; i++) {
        const idx = startIdx + i;
        const param = params[idx];

        const y = 14 + i * 10;

        // Section navigation item (e.g. Wave/Pitch/...): show "Label >", no value.
        if (param && typeof param === 'object' && param.level) {
            const navLabel = (param.label || param.level) + ' >';
            if (idx === menuIndex) {
                display.fillRect(0, y - 1, SCREEN_WIDTH, 10, 1);
                display.drawText(4, y, navLabel, 0);
            } else {
                display.drawText(4, y, navLabel, 1);
            }
            continue;
        }

        const paramKey = typeof param === 'string' ? param : param.key;
        const meta = getParamMeta(paramKey);

        const t1 = Date.now();
        const value = getParamValue(paramKey);
        getParamTime += Date.now() - t1;

        const label = meta.name || paramKey;
        const valueStr = formatValue(value, meta);

        const t2 = Date.now();
        if (idx === menuIndex) {
            display.fillRect(0, y - 1, SCREEN_WIDTH, 10, 1);
            display.drawText(4, y, label, 0);
            display.drawText(SCREEN_WIDTH - valueStr.length * 6 - 4, y, valueStr, 0);

            if (editingParam === paramKey) {
                // Draw editing indicator
                display.drawRect(SCREEN_WIDTH - valueStr.length * 6 - 6, y - 2, valueStr.length * 6 + 4, 12, 0);
            }
        } else {
            display.drawText(4, y, label, 1);
            display.drawText(SCREEN_WIDTH - valueStr.length * 6 - 4, y, valueStr, 1);
        }
        drawTime += Date.now() - t2;
    }

    const totalTime = Date.now() - t0;
    if (totalTime > 5) {
        console.log("drawChildSelector: total=" + totalTime + "ms, getParam=" + getParamTime + "ms, draw=" + drawTime + "ms");
    }
}

function drawMenu() {
    // Header
    const label = currentLevel.label || 'Menu';
    display.drawText(0, 0, label, 1);
    display.drawLine(0, 10, SCREEN_WIDTH, 10, 1);

    // Menu items
    const params = currentLevel.params || [];
    const visibleItems = 5;
    const startIdx = Math.max(0, menuIndex - 2);

    for (let i = 0; i < visibleItems && startIdx + i < params.length; i++) {
        const idx = startIdx + i;
        const item = params[idx];
        const y = 14 + i * 10;

        let label, valueStr = '';

        if (typeof item === 'string') {
            // Simple param key
            const meta = getParamMeta(item);
            label = meta.name || item;
            valueStr = formatValue(getParamValue(item), meta);
        } else if (item.level) {
            // Navigation item
            label = item.label + ' >';
        } else if (item.key) {
            // Param with label
            const meta = getParamMeta(item.key);
            label = item.label || meta.name || item.key;
            valueStr = formatValue(getParamValue(item.key), meta);
        }

        if (idx === menuIndex) {
            display.fillRect(0, y - 1, SCREEN_WIDTH, 10, 1);
            display.drawText(4, y, label, 0);
            if (valueStr) {
                display.drawText(SCREEN_WIDTH - valueStr.length * 6 - 4, y, valueStr, 0);
            }

            // Editing indicator
            const paramKey = typeof item === 'string' ? item : item.key;
            if (editingParam === paramKey) {
                display.drawRect(SCREEN_WIDTH - valueStr.length * 6 - 6, y - 2, valueStr.length * 6 + 4, 12, 0);
            }
        } else {
            display.drawText(4, y, label, 1);
            if (valueStr) {
                display.drawText(SCREEN_WIDTH - valueStr.length * 6 - 4, y, valueStr, 1);
            }
        }
    }
}

function formatValue(value, meta) {
    if (meta.options) {
        return meta.options[value] || String(value);
    }
    // Pan-style display: 0 is center, negatives L, positives R (e.g. L23 / C / R12).
    if (meta.display === 'pan') {
        if (value === 0) return 'C';
        return value < 0 ? 'L' + (-value) : 'R' + value;
    }
    // Signed/centered params: show an explicit + on positives so polarity reads
    // clearly (e.g. pitch +3, env depth -12). The C plugin already returns the
    // value pre-centered, so no offset math is needed here.
    if (meta.display === 'signed' && value > 0) {
        return '+' + value;
    }
    return String(value);
}

/* === Knob Overlay === */
// Drawn on top of any screen when a knob was recently turned.
// Shows: param name (line 1) and value with range context (line 2).
// Disappears after KNOB_OVERLAY_TICKS ticks of no knob activity.
function drawKnobOverlay() {
    if (!knobOverlayParam) return;

    const meta = getParamMeta(knobOverlayParam);
    const name = meta.name || knobOverlayParam;
    const valueStr = formatValue(knobOverlayValue, meta);

    // Build range hint, e.g. "92 [0..127]" — keep total <=21 chars
    const rangeHint = (meta.min !== undefined && meta.max !== undefined)
        ? valueStr + ' [' + meta.min + '..' + meta.max + ']'
        : valueStr;

    // Overlay box: 4px padding, centered vertically around y=24
    const boxW = SCREEN_WIDTH - 8;      // 120px wide
    const boxX = 4;
    const boxY = 18;
    const boxH = 26;

    // White fill + black border
    display.fillRect(boxX, boxY, boxW, boxH, 1);
    display.drawRect(boxX, boxY, boxW, boxH, 0);

    // Line 1: param name (truncate to 19 chars to stay inside box)
    const displayName = name.length > 19 ? name.substring(0, 19) : name;
    const nameX = boxX + 4;
    display.drawText(nameX, boxY + 3, displayName, 0);

    // Line 2: value + range (truncate to 19 chars)
    const displayRange = rangeHint.length > 19 ? rangeHint.substring(0, 19) : rangeHint;
    display.drawText(nameX, boxY + 14, displayRange, 0);
}

/* === Tick === */
function tick() {
    tickCount++;

    // Retry loading hierarchy if not yet loaded
    if (!loadingComplete && tickCount % 30 === 0) {
        loadHierarchy();
    }

    // Refresh data periodically for list browsers
    if (tickCount % 10 === 0) {
        if (currentLevel && currentLevel.list_param) {
            const newCount = parseInt(host_module_get_param(currentLevel.count_param) || '0');
            const newName = host_module_get_param(currentLevel.name_param) || '';
            if (newCount !== listCount || newName !== listName) {
                listCount = newCount;
                listName = newName;
                listIndex = parseInt(host_module_get_param(currentLevel.list_param) || '0');
                needsRedraw = true;
            }
        }
    }

    if (needsRedraw) {
        draw();
        needsRedraw = false;
    }
}

/* === MIDI Handling === */
function onMidiMessageInternal(msg) {
    if (isCapacitiveTouchMessage(msg)) return;

    if (msg.length >= 3 && (msg[0] & 0xF0) === 0xB0) {
        const cc = msg[1];
        const value = msg[2];

        // Encoder knobs
        const knobIdx = ENCODER_CCS.indexOf(cc);
        if (knobIdx >= 0 && value !== 64) {
            const delta = decodeDelta(value);
            handleKnob(knobIdx, delta);
            return;
        }

        handleCC(cc, value);
    }
}

function onMidiMessageExternal(msg) {
    // Forward external MIDI to plugin
    host_module_send_midi(msg, 1);
}

/* === Exports === */
globalThis.init = init;
globalThis.tick = tick;
globalThis.onMidiMessageInternal = onMidiMessageInternal;
globalThis.onMidiMessageExternal = onMidiMessageExternal;
