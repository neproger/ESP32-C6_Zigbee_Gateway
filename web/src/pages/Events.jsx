import { useCallback, useEffect, useMemo, useRef, useState } from 'react'

function shortToHex(shortAddr) {
  const v = Number(shortAddr ?? 0)
  if (!Number.isFinite(v)) return '0x0'
  return `0x${v.toString(16)}`
}

function msToUptime(tsMs) {
  const v = Number(tsMs ?? 0)
  if (!Number.isFinite(v)) return ''
  return `+${(v / 1000).toFixed(3)}s`
}

export default function Events() {
  const [events, setEvents] = useState([])
  const [lastId, setLastId] = useState(0)
  const [paused, setPaused] = useState(false)
  const [status, setStatus] = useState('')

  const lastIdRef = useRef(0)

  const load = useCallback(async () => {
    try {
      const since = lastIdRef.current ?? 0
      const r = await fetch(`/api/events?since=${encodeURIComponent(since)}&limit=64`)
      if (!r.ok) throw new Error(`GET /api/events failed: ${r.status}`)
      const data = await r.json()
      const incoming = Array.isArray(data?.events) ? data.events : []

      const nextLast = Number(data?.last_id ?? since)
      if (Number.isFinite(nextLast) && nextLast >= since) {
        lastIdRef.current = nextLast
        setLastId(nextLast)
      }

      if (incoming.length > 0) {
        setEvents((prev) => {
          const merged = [...(Array.isArray(prev) ? prev : []), ...incoming]
          const max = 200
          return merged.length > max ? merged.slice(merged.length - max) : merged
        })
      }

      setStatus('')
    } catch (e) {
      setStatus(String(e?.message ?? e))
    }
  }, [])

  useEffect(() => {
    if (paused) return
    load()
    const t = setInterval(load, 1000)
    return () => clearInterval(t)
  }, [paused, load])

  const sortedEvents = useMemo(() => {
    const items = Array.isArray(events) ? [...events] : []
    items.sort((a, b) => Number(a?.id ?? 0) - Number(b?.id ?? 0))
    return items
  }, [events])

  const clear = useCallback(() => {
    setEvents([])
  }, [])

  return (
    <div className="page">
      <div className="header">
        <div>
          <h1>Events</h1>
          <div className="muted">In-memory gateway event log (polls /api/events every 1s).</div>
        </div>
        <div className="row">
          <button onClick={() => setPaused((p) => !p)}>{paused ? 'Resume' : 'Pause'}</button>
          <button onClick={load} disabled={paused}>
            Refresh
          </button>
          <button onClick={clear}>Clear</button>
          <div className="muted">last_id: {lastId}</div>
        </div>
      </div>

      {status ? <div className="status">{status}</div> : null}

      <div className="card scroll height-100">
        <table>
          <thead>
            <tr>
              <th>ID</th>
              <th>Time</th>
              <th>Type</th>
              <th>Source</th>
              <th>UID</th>
              <th>Short</th>
              <th>Message</th>
            </tr>
          </thead>
          <tbody>
            {sortedEvents.length === 0 ? (
              <tr>
                <td colSpan={7} className="muted">
                  No events yet. Try enabling permit join, reconnecting Wi-Fi, or rebooting.
                </td>
              </tr>
            ) : (
              sortedEvents.map((e) => (
                <tr key={String(e?.id ?? Math.random())}>
                  <td>
                    <code>{String(e?.id ?? '')}</code>
                  </td>
                  <td className="muted">{msToUptime(e?.ts_ms)}</td>
                  <td>{String(e?.type ?? '')}</td>
                  <td className="muted">{String(e?.source ?? '')}</td>
                  <td>
                    <code>{String(e?.device_uid ?? '')}</code>
                  </td>
                  <td>
                    <code>{shortToHex(e?.short_addr)}</code>
                  </td>
                  <td className="mono">{String(e?.msg ?? '')}</td>
                </tr>
              ))
            )}
          </tbody>
        </table>
      </div>
    </div>
  )
}

