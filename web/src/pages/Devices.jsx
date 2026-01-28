import { useCallback, useEffect, useMemo, useState } from 'react'

function capsToText(device) {
  const caps = []
  if (device?.has_onoff) caps.push('onoff')
  if (device?.has_button) caps.push('button')
  return caps.join(', ')
}

function shortToHex(shortAddr) {
  const v = Number(shortAddr ?? 0)
  if (!Number.isFinite(v)) return '0x0'
  return `0x${v.toString(16)}`
}

export default function Devices() {
  const [devices, setDevices] = useState([])
  const [loading, setLoading] = useState(false)
  const [status, setStatus] = useState('')

  const sortedDevices = useMemo(() => {
    const items = Array.isArray(devices) ? [...devices] : []
    items.sort((a, b) => String(a?.device_uid ?? '').localeCompare(String(b?.device_uid ?? '')))
    return items
  }, [devices])

  const loadDevices = useCallback(async () => {
    setLoading(true)
    setStatus('')
    try {
      const r = await fetch('/api/devices')
      if (!r.ok) throw new Error(`GET /api/devices failed: ${r.status}`)
      const data = await r.json()
      setDevices(Array.isArray(data) ? data : [])
    } catch (e) {
      setStatus(String(e?.message ?? e))
    } finally {
      setLoading(false)
    }
  }, [])

  const permitJoin = useCallback(async () => {
    setStatus('Permit join: requesting…')
    try {
      const r = await fetch('/api/network/permit_join?seconds=180', { method: 'POST' })
      if (!r.ok) throw new Error(`POST /api/network/permit_join failed: ${r.status}`)
      const data = await r.json()
      const seconds = Number(data?.seconds ?? 180)
      setStatus(`Permit join enabled for ${Number.isFinite(seconds) ? seconds : 180}s`)
    } catch (e) {
      setStatus(String(e?.message ?? e))
    }
  }, [])

  useEffect(() => {
    loadDevices()
  }, [loadDevices])

  return (
    <div className="page">
      <div className="header">
        <div>
          <h1>Devices</h1>
          <div className="muted">Zigbee devices that joined/rejoined (DEVICE_ANNCE).</div>
        </div>
        <div className="row">
          <button onClick={loadDevices} disabled={loading}>
            {loading ? 'Refreshing…' : 'Refresh'}
          </button>
          <button onClick={permitJoin}>Scan new devices (permit join)</button>
        </div>
      </div>

      {status ? <div className="status">{status}</div> : null}

      <div className="card">
        <table>
          <thead>
            <tr>
              <th>UID</th>
              <th>Name</th>
              <th>Short</th>
              <th>Caps</th>
            </tr>
          </thead>
          <tbody>
            {sortedDevices.length === 0 ? (
              <tr>
                <td colSpan={4} className="muted">
                  No devices yet. Click “Scan new devices (permit join)”, then pair a Zigbee device.
                </td>
              </tr>
            ) : (
              sortedDevices.map((d) => (
                <tr key={String(d?.device_uid ?? Math.random())}>
                  <td>
                    <code>{String(d?.device_uid ?? '')}</code>
                  </td>
                  <td>{String(d?.name ?? '')}</td>
                  <td>
                    <code>{shortToHex(d?.short_addr)}</code>
                  </td>
                  <td>{capsToText(d)}</td>
                </tr>
              ))
            )}
          </tbody>
        </table>
      </div>
    </div>
  )
}

