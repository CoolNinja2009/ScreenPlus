/* The page's end of PointerWatch.qml: raises the events a real pointer would, on whatever is under it. */
(function () {
    var over = null, pressed = null, lastX = 0, lastY = 0;

    function fire(el, name, x, y, button, buttons, related, bubbles) {
        var init = {
            bubbles: bubbles, cancelable: true, composed: true, view: window,
            clientX: x, clientY: y, screenX: x, screenY: y, movementX: x - lastX, movementY: y - lastY,
            button: button, buttons: buttons, relatedTarget: related || null
        };
        if (name.indexOf('pointer') === 0) {
            init.pointerId = 1;
            init.pointerType = 'mouse';
            init.isPrimary = true;
            el.dispatchEvent(new PointerEvent(name, init));
        } else {
            el.dispatchEvent(new MouseEvent(name, init));
        }
    }
    function both(el, what, x, y, button, buttons, related, bubbles) {
        fire(el, 'pointer' + what, x, y, button, buttons, related, bubbles !== false);
        fire(el, 'mouse' + what, x, y, button, buttons, related, bubbles !== false);
    }

    window.__spPointer = function (type, x, y, button, buttons) {
        if (type === 'leave') {
            if (over) {
                both(over, 'out', x, y, 0, 0, null);
                both(over, 'leave', x, y, 0, 0, null, false);
            }
            over = null;
            return;
        }
        var el = document.elementFromPoint(x, y) || document.documentElement;
        if (el !== over) {
            if (over) {
                both(over, 'out', x, y, 0, buttons, el);
                both(over, 'leave', x, y, 0, buttons, el, false);
            }
            both(el, 'over', x, y, 0, buttons, over);
            both(el, 'enter', x, y, 0, buttons, over, false);
            over = el;
        }
        if (type === 'move') {
            both(el, 'move', x, y, 0, buttons);
        } else if (type === 'down') {
            pressed = el;
            both(el, 'down', x, y, button, buttons);
        } else if (type === 'up') {
            both(el, 'up', x, y, button, buttons);
            if (pressed === el && button === 0) fire(el, 'click', x, y, 0, 0, null, true);
            pressed = null;
        }
        lastX = x;
        lastY = y;
    };
})();
