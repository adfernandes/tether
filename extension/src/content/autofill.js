// Content script for browsers (Chrome/Firefox)
// This runs in the context of webpages and looks for OTP input fields

export function findOtpInputs(doc) {
  const inputs = [...doc.querySelectorAll('input')];

  return inputs.filter(input => {
    const attrs = [input.id, input.name, input.placeholder, input.className, input.type]
      .join(' ').toLowerCase();

    // Reject obvious non-OTP fields immediately
    if (/\b(zip|search|password|email|phone)\b/.test(attrs)) return false;

    // Tier 1: explicit standard attribute
    if (input.autocomplete === 'one-time-code') return true;

    // Tier 2: type/inputmode signals
    if (input.inputMode === 'numeric') return scoreInput(input) > 5;
    if (input.type === 'tel' || input.type === 'number') return scoreInput(input) > 5;

    // Tier 3: attribute keyword matching
    if (/\b(otp|passcode|verif|token|pin|2fa)\b/.test(attrs)) return true;

    // Tier 4: maxlength in OTP range (4–8)
    const ml = parseInt(input.maxLength);
    if (ml >= 4 && ml <= 8) return scoreInput(input) > 3;

    // Tier 5: fallback for generic text inputs based on label/context
    if (input.type === 'text' || !input.type) {
      if (scoreInput(input) >= 10) return true;
    }

    return false;
  });
}

export function scoreInput(input) {
  let score = 0;
  let labelText = input.labels?.[0]?.textContent || '';
  if (!labelText && input.id) {
    try {
      const doc = input.ownerDocument || document;
      const labelEl = doc.querySelector(`label[for="${input.id}"]`);
      if (labelEl) labelText = labelEl.textContent;
    } catch (e) {}
  }
  if (!labelText) {
    labelText = input.closest('form')?.querySelector('label')?.textContent || '';
  }

  if (/code|otp|verify|token|pin/i.test(labelText)) score += 10;

  // Surrounding text in parent
  const parentText = input.parentElement?.textContent?.toLowerCase() || '';
  if (/enter.*code|verification|one.time/i.test(parentText)) score += 5;

  return score;
}

export function detectSplitOtp(doc) {
  const inputs = [...doc.querySelectorAll('input')]
    .filter(i => i.type === 'text' || i.type === 'tel' || i.type === 'number' || !i.type);

  // Strategy 1: exact maxlength=1 (standard split OTP)
  const singles = inputs.filter(i => i.maxLength === 1);
  if (singles.length >= 4 && singles.length <= 8) {
    return singles;
  }

  // Strategy 2: Clustered inputs under same parent with OTP keywords (e.g. Walmart)
  const parentMap = new Map();
  inputs.forEach(input => {
    const p = input.parentElement;
    if (p) {
      if (!parentMap.has(p)) parentMap.set(p, []);
      parentMap.get(p).push(input);
    }
  });

  for (const group of parentMap.values()) {
    if (group.length >= 4 && group.length <= 8) {
      const otpScore = group.reduce((acc, input) => {
        const attrs = [input.id, input.name, input.className, input.getAttribute('aria-label')]
          .join(' ').toLowerCase();
        if (/\botp\b|\bcode\b|verif|digit/.test(attrs)) return acc + 1;
        return acc;
      }, 0);

      // If at least half the inputs in the group look like OTP inputs
      if (otpScore >= group.length / 2) return group;
    }
  }

  return null;
}

function setNativeValue(el, value) {
  const setter = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(el), 'value')?.set;
  if (setter) setter.call(el, value);
  else el.value = value;
}

function fireInputEvents(el) {
  const view = el.ownerDocument?.defaultView;
  const Ev = view?.Event || Event;
  el.dispatchEvent(new Ev('input', { bubbles: true }));
  el.dispatchEvent(new Ev('change', { bubbles: true }));
}

// Put focus on the filled field so screen readers announce the code and Enter
// submits it. Focus is never pulled away from a field the user is typing in.
function focusFilled(doc, target, fields) {
  const active = doc.activeElement;
  if (active && active !== doc.body && !fields.includes(active)) return;
  target.focus?.();
}

// A field is only eligible for filling if it is actually rendered. This stops a
// malicious page from planting a hidden "OTP" input to siphon a real code.
export function isFieldVisible(el) {
  if (!el || el.hidden || el.type === 'hidden') return false;
  for (let n = el; n && n.nodeType === 1; n = n.parentElement) {
    if (n.hidden) return false;
    const s = n.style;
    if (s && (s.display === 'none' || s.visibility === 'hidden' || s.visibility === 'collapse')) {
      return false;
    }
  }
  // In a real browser, also reject elements with no rendered box. jsdom has no
  // layout engine, so only enforce this where getComputedStyle is meaningful.
  const view = el.ownerDocument?.defaultView;
  if (view && typeof view.getComputedStyle === 'function') {
    try {
      const cs = view.getComputedStyle(el);
      if (cs && (cs.display === 'none' || cs.visibility === 'hidden' || cs.visibility === 'collapse')) {
        return false;
      }
    } catch (e) { /* ignore */ }
  }
  return true;
}

// The page must be the tab the user is actually looking at before we request or
// fill an OTP. Engines without visibility reporting are treated as visible.
export function isPageVisible(doc) {
  const vs = doc?.visibilityState;
  return !vs || vs === 'visible';
}

// codes this page has already filled
const filledOtpIds = new Set();

export function _resetFilledOtps() { filledOtpIds.clear(); } // tests

// fill `msg.otp` into whatever OTP fields this document has.
export function handleFillOtp(doc, msg, io = {}) {
  const otp = msg?.otp?.toString();
  if (!otp) return { filled: false };

  const id = msg.otp_id || 0;
  if (id && filledOtpIds.has(id)) return { filled: false };

  const splitFields = detectSplitOtp(doc);
  if (splitFields && splitFields.every(isFieldVisible)) {
    for (let i = 0; i < Math.min(otp.length, splitFields.length); i++) {
      setNativeValue(splitFields[i], otp[i]);
      // dispatch events so react/vue/angular crap pick up the change
      fireInputEvents(splitFields[i]);
    }
    focusFilled(doc, splitFields[Math.min(otp.length, splitFields.length) - 1], splitFields);
    if (id) filledOtpIds.add(id);
    io.onFilled?.(id);
    return { filled: true };
  }

  const regularFields = findOtpInputs(doc).filter(isFieldVisible);
  // fill when empty, or overwrite a stale/partial value
  if (regularFields.length > 0 && regularFields[0].value !== otp) {
    setNativeValue(regularFields[0], otp);
    fireInputEvents(regularFields[0]);
    focusFilled(doc, regularFields[0], regularFields);
    if (id) filledOtpIds.add(id);
    io.onFilled?.(id);
    return { filled: true };
  }

  return { filled: false };
}

// ---------------------------------------------------------------------------
// Browser-only code - only runs when loaded in actual browser context
// ---------------------------------------------------------------------------
if (typeof document === 'undefined') {
  // Test environment: skip browser-only code
} else {
let otpInterval = null;
// last code id this page was offered
let lastSeenOtpId = 0;

function requestOtp() {
  chrome.runtime.sendMessage({
    action: "request_otp_for_site",
    url: window.location.hostname
  });
}

function consumeOtp(otpId) {
  chrome.runtime.sendMessage({ action: "consume_otp", otp_id: otpId || 0 });
}

function hasOtpFields() {
  if (detectSplitOtp(document)) return true;
  return findOtpInputs(document).length > 0;
}

// Check every 2 seconds for up to 2 minutes
let otpFlowStarted = false;
function startOtpFlow() {
  // Never request codes while the tab is hidden in the background.
  if (!isPageVisible(document)) return;
  if (otpFlowStarted) return;
  otpFlowStarted = true;
  requestOtp();

  let attempts = 0;
  otpInterval = setInterval(() => {
    // While the tab is hidden, neither request nor fill. Don't burn attempts.
    if (!isPageVisible(document)) return;

    attempts++;
    const splits = detectSplitOtp(document);
    const regular = findOtpInputs(document);

    // stop trying after 2 minutes or if the user manually filled the field
    const userFilled = (splits && splits[0].value !== "") ||
                       (regular.length > 0 && regular[0].value.length > 3);
    if (attempts > 60 || userFilled) {
      clearInterval(otpInterval);
      if (userFilled) consumeOtp(lastSeenOtpId);
      return;
    }
    requestOtp();
  }, 2000);
}

const otpPresentAtLoad = hasOtpFields();
if (otpPresentAtLoad) {
  startOtpFlow();
} else {
  const observer = new MutationObserver(() => {
    if (hasOtpFields()) {
      observer.disconnect();
      startOtpFlow();
    }
  });
  observer.observe(document.documentElement, { childList: true, subtree: true });
  // ponytail: bound the watcher so it doesn't run forever on non-OTP pages
  setTimeout(() => observer.disconnect(), 120000);
}

// If the page was hidden when OTP fields appeared, start the flow once it
// becomes visible.
document.addEventListener('visibilitychange', () => {
  if (hasOtpFields()) startOtpFlow();
});

// listen for OTPs sent from the daemon
chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request.action !== "fill_otp") return;

  // Only fill into a page the user is actually looking at.
  if (!isPageVisible(document)) {
    sendResponse({ filled: false });
    return;
  }

  lastSeenOtpId = request.otp_id || lastSeenOtpId;
  const result = handleFillOtp(document, request, {
    onFilled: () => {
      if (otpInterval) clearInterval(otpInterval);
    }
  });
  sendResponse(result);
});
}
