/* Customise recordings on Plasma (wallpaper_api.c's wprec_*): the service hands over {"we", "look", "props"} with the
 * samples already smoothed, and this plays them on the same wall clock as the window's graphs (epoch = unix ms of
 * sample 0, REC_RATE samples a second at `speed`). The look drives the plugin's grading; properties go to the page as
 * the Lively or Wallpaper Engine API wants them, three times a second less often than the look (pages may rebuild). */
import QtQuick

Item {
    id: rec
    property string json             /* the recording (desktop_kde.c's recording_json) */
    property bool running: false
    readonly property int rate: 30

    readonly property var parsed: { try { return json ? JSON.parse(json) : null } catch (e) { return null } }
    readonly property bool hasLook: !!(parsed && parsed.look && parsed.look.n > 1)
    readonly property bool hasProps: !!(parsed && parsed.props && parsed.props.n > 1)

    /* The look now: a value per recorded key ("brightness", ...), NaN for keys not recorded. */
    property var look: ({})
    signal propsScript(string js)

    function pos(r) {
        const p = (((r.paused > 0 ? r.paused : Date.now()) - r.epoch) / 1000 * r.speed * rate) % r.n
        return p < 0 ? p + r.n : p
    }

    function sample(t, n, p) {
        const a = Math.floor(p) % n, b = (a + 1) % n, f = p - Math.floor(p), va = t.v[a], vb = t.v[b]
        if (t.kind === 0) return va + (vb - va) * f
        if (t.kind === 3) {
            let o = 0
            for (let s = 0; s <= 16; s += 8) {
                const x = (va >> s) & 255, y = (vb >> s) & 255
                o |= Math.round(x + (y - x) * f) << s
            }
            return o
        }
        return va
    }

    function hex(n, w) { return n.toString(16).toUpperCase().padStart(w, "0") }

    /* The value the page's API expects, as wallpaper_api.c's wprec_value makes it. */
    function value(t, n, p) {
        const x = sample(t, n, p)
        if (t.kind === 1) return x >= 0.5
        if (t.kind === 2) return t.opts ? t.opts[Math.max(0, Math.min(t.opts.length - 1, Math.round(x)))] : null
        if (t.kind === 3) {
            const r = (x >> 16) & 255, g = (x >> 8) & 255, b = x & 255
            if (t.style === 2) return [r, g, b].map(c => +(c / 255).toPrecision(4)).join(" ")
            return (t.style === 1 ? "0x" : "#") + hex(x & 0xFFFFFF, 6)
        }
        const step = t.step > 0 ? t.step : 0.01, d = Math.max(0, -Math.floor(Math.log10(step) + 1e-9))
        return +(Math.round(x / step) * step).toFixed(d)
    }

    property int ticks: 0
    property var sent: ({})          /* key -> JSON of the value the page has */
    Timer {
        interval: 33
        repeat: true
        running: rec.running && (rec.hasLook || rec.hasProps)
        onTriggered: {
            const P = rec.parsed
            if (rec.hasLook) {
                const r = P.look, p = rec.pos(r), o = {}
                for (const t of r.tracks) o[t.key] = rec.sample(t, r.n, p)
                rec.look = o
            }
            if (rec.hasProps && rec.ticks++ % 3 === 0) {
                const r = P.props, p = rec.pos(r), changed = {}
                let count = 0
                for (const t of r.tracks) {                 /* only what the page doesn't have yet */
                    const v = rec.value(t, r.n, p), s = JSON.stringify(v)
                    if (rec.sent[t.key] === s) continue
                    rec.sent[t.key] = s
                    changed[t.key] = v
                    count++
                }
                if (!count) return
                let js = ""
                if (P.we) {
                    const o = {}
                    for (const k in changed) o[k] = { value: changed[k] }
                    js = "(function(){var l=window.wallpaperPropertyListener;try{if(l&&l.applyUserProperties)l.applyUserProperties("
                       + JSON.stringify(o) + ")}catch(e){}})()"
                } else {
                    js = "(function(){var f=window.livelyPropertyListener;if(typeof f!=='function')return;"
                    for (const k in changed) js += "try{f(" + JSON.stringify(k) + "," + JSON.stringify(changed[k]) + ")}catch(e){}"
                    js += "})()"
                }
                rec.propsScript(js)
            }
        }
    }
    onJsonChanged: { look = ({}); sent = ({}) }
    function reset() { sent = ({}) }   /* a new page: it has nothing yet */
}
