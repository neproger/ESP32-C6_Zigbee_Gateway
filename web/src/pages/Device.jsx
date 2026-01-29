import { Link, useParams } from 'react-router-dom'
import { useCallback, useEffect, useMemo, useState } from 'react'
import { describeAttr, describeCluster, describeDeviceId, describeProfile, formatSensorValue, hex16 } from '../zigbee/zcl.js'

function renderSensorValue(s) {
  return formatSensorValue(s)
}

function listToText(v) {
  return Array.isArray(v) ? v.map((x) => String(x ?? '')).filter(Boolean).join(', ') : ''
}

function wsUrl(path) {
  const proto = window.location.protocol === 'https:' ? 'wss' : 'ws'
  return `${proto}://${window.location.host}${path}`
}

async function wsReq(method, params, { timeoutMs = 3500 } = {}) {
  const id = `${Date.now()}-${Math.random().toString(16).slice(2)}`
  const ws = new WebSocket(wsUrl('/ws'))

  const waitForOpen = new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('WebSocket timeout (open)')), timeoutMs)
    ws.onopen = () => {
      clearTimeout(t)
      resolve()
    }
    ws.onerror = () => {
      clearTimeout(t)
      reject(new Error('WebSocket error'))
    }
  })

  await waitForOpen

  const waitForRsp = new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('WebSocket timeout (rsp)')), timeoutMs)
    ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(String(ev?.data ?? ''))
        if (msg?.t !== 'rsp') return
        if (String(msg?.id ?? '') !== id) return
        clearTimeout(t)
        resolve(msg)
      } catch {
        // ignore
      }
    }
    ws.onerror = () => {
      clearTimeout(t)
      reject(new Error('WebSocket error'))
    }
    ws.onclose = () => {
      // don't reject here; timeout will handle it
    }
  })

  ws.send(JSON.stringify({ t: 'req', id, m: method, p: params }))
  const rsp = await waitForRsp
  try {
    ws.close()
  } catch { }

  if (!rsp?.ok) {
    throw new Error(String(rsp?.err ?? 'request failed'))
  }
  return rsp?.res ?? null
}

function clamp01(v) {
  if (!Number.isFinite(v)) return 0
  return Math.max(0, Math.min(1, v))
}

function srgbToLinear(v) {
  const x = v / 255
  return x <= 0.04045 ? x / 12.92 : ((x + 0.055) / 1.055) ** 2.4
}

function rgbHexToXy(hex) {
  const s = String(hex ?? '').replace('#', '').trim()
  if (s.length !== 6) return { x: 0, y: 0 }
  const r = parseInt(s.slice(0, 2), 16)
  const g = parseInt(s.slice(2, 4), 16)
  const b = parseInt(s.slice(4, 6), 16)
  if (![r, g, b].every((n) => Number.isFinite(n))) return { x: 0, y: 0 }

  // Convert sRGB -> linear -> XYZ (D65) -> xy (CIE 1931).
  const rl = srgbToLinear(r)
  const gl = srgbToLinear(g)
  const bl = srgbToLinear(b)

  const X = rl * 0.4124 + gl * 0.3576 + bl * 0.1805
  const Y = rl * 0.2126 + gl * 0.7152 + bl * 0.0722
  const Z = rl * 0.0193 + gl * 0.1192 + bl * 0.9505
  const sum = X + Y + Z
  const x = sum > 0 ? X / sum : 0
  const y = sum > 0 ? Y / sum : 0

  return {
    x: Math.round(clamp01(x) * 65535),
    y: Math.round(clamp01(y) * 65535),
  }
}

export default function Device() {
  const { uid } = useParams()
  const [device, setDevice] = useState(null)
  const [endpoints, setEndpoints] = useState([])
  const [sensors, setSensors] = useState([])
  const [state, setState] = useState({})
  const [levelByEndpoint, setLevelByEndpoint] = useState(() => new Map())
  const [colorByEndpoint, setColorByEndpoint] = useState(() => new Map())
  const [tempKByEndpoint, setTempKByEndpoint] = useState(() => new Map())
  const [status, setStatus] = useState('')

  const load = useCallback(async () => {
    const u = String(uid ?? '')
    if (!u) return
    setStatus('')
    try {
      const [dr, epr, sr, st] = await Promise.all([
        fetch(`/api/devices`),
        fetch(`/api/endpoints?uid=${encodeURIComponent(u)}`),
        fetch(`/api/sensors?uid=${encodeURIComponent(u)}`),
        fetch(`/api/state?uid=${encodeURIComponent(u)}`),
      ])
      if (!dr.ok) throw new Error(`GET /api/devices failed: ${dr.status}`)
      if (!epr.ok) throw new Error(`GET /api/endpoints failed: ${epr.status}`)
      if (!sr.ok) throw new Error(`GET /api/sensors failed: ${sr.status}`)
      if (!st.ok) throw new Error(`GET /api/state failed: ${st.status}`)
      const devs = await dr.json()
      const d = Array.isArray(devs) ? devs.find((x) => String(x?.device_uid ?? '') === u) : null
      setDevice(d ?? null)
      const epData = await epr.json()
      const sData = await sr.json()
      const stData = await st.json()
      setEndpoints(Array.isArray(epData) ? epData : [])
      setSensors(Array.isArray(sData) ? sData : [])
      setState(stData?.state && typeof stData.state === 'object' ? stData.state : {})
    } catch (e) {
      setStatus(String(e?.message ?? e))
    }
  }, [uid])

  useEffect(() => {
    load()
  }, [load])

  const sortedEndpoints = useMemo(() => {
    const items = Array.isArray(endpoints) ? [...endpoints] : []
    items.sort((a, b) => Number(a?.endpoint ?? 0) - Number(b?.endpoint ?? 0))
    return items
  }, [endpoints])

  const sortedSensors = useMemo(() => {
    const items = Array.isArray(sensors) ? [...sensors] : []
    items.sort((a, b) => {
      const ak = `${a?.endpoint ?? 0}:${a?.cluster_id ?? 0}:${a?.attr_id ?? 0}`
      const bk = `${b?.endpoint ?? 0}:${b?.cluster_id ?? 0}:${b?.attr_id ?? 0}`
      return ak.localeCompare(bk)
    })
    return items
  }, [sensors])

  const hasAccept = useCallback((ep, prefix) => {
    const arr = Array.isArray(ep?.accepts) ? ep.accepts : []
    return arr.some((x) => String(x ?? '').startsWith(prefix))
  }, [])

  const sendOnOff = useCallback(
    async (endpoint, cmd) => {
      const u = String(uid ?? '')
      if (!u) return
      setStatus('Sending...')
      try {
        await wsReq('devices.onoff', { uid: u, endpoint, cmd })
        await load()
        setStatus('')
      } catch (e) {
        setStatus(String(e?.message ?? e))
      }
    },
    [uid, load],
  )

  const sendLevel = useCallback(
    async (endpoint) => {
      const u = String(uid ?? '')
      const level = Number(levelByEndpoint.get(Number(endpoint)) ?? 0)
      if (!u) return
      if (!Number.isFinite(level)) return
      setStatus('Sending...')
      try {
        await wsReq('devices.level', { uid: u, endpoint, level: Math.max(0, Math.min(254, Math.round(level))), transition_ms: 300 })
        setStatus('')
      } catch (e) {
        setStatus(String(e?.message ?? e))
      }
    },
    [uid, levelByEndpoint],
  )

  const sendColor = useCallback(
    async (endpoint) => {
      const u = String(uid ?? '')
      const hex = String(colorByEndpoint.get(Number(endpoint)) ?? '#ffffff')
      if (!u) return
      const { x, y } = rgbHexToXy(hex)
      setStatus('Sending...')
      try {
        await wsReq('devices.color_xy', { uid: u, endpoint, x, y, transition_ms: 400 })
        setStatus('')
      } catch (e) {
        setStatus(String(e?.message ?? e))
      }
    },
    [uid, colorByEndpoint],
  )

  const sendTemp = useCallback(
    async (endpoint) => {
      const u = String(uid ?? '')
      const k = Number(tempKByEndpoint.get(Number(endpoint)) ?? 3000)
      if (!u || !Number.isFinite(k) || k <= 0) return
      const mireds = Math.round(1_000_000 / k)
      setStatus('Sending...')
      try {
        await wsReq('devices.color_temp', { uid: u, endpoint, mireds, transition_ms: 600 })
        setStatus('')
      } catch (e) {
        setStatus(String(e?.message ?? e))
      }
    },
    [uid, tempKByEndpoint],
  )

  return (
    <div className="page">
      <div className="header">
        <div>
          <h1>Device</h1>
          <div className="muted">
            {device?.name ? (
              <>
                <span>{String(device.name)}</span> <span className="muted">·</span>{' '}
              </>
            ) : null}
            <code>{String(uid ?? '')}</code>
          </div>
        </div>
        <div className="row">
          <button onClick={load}>Refresh</button>
          <Link className="navlink" to="/devices">
            Back
          </Link>
        </div>
      </div>

      {status ? <div className="status">{status}</div> : null}

      <div className="card">
        <table>
          <thead>
            <tr>
              <th>Endpoint</th>
              <th>Kind</th>
              <th>Profile</th>
              <th>Device</th>
              <th>Controls</th>
              <th>Accepts</th>
              <th>Emits</th>
              <th>Reports</th>
              <th>In clusters</th>
              <th>Out clusters</th>
            </tr>
          </thead>
          <tbody>
            {sortedEndpoints.length === 0 ? (
              <tr>
                <td colSpan={10} className="muted">
                  No endpoints discovered yet.
                </td>
              </tr>
            ) : (
              sortedEndpoints.map((e) => (
                <tr key={String(e?.endpoint ?? Math.random())}>
                  <td>
                    <code>{String(e?.endpoint ?? '')}</code>
                  </td>
                  <td className="muted">{String(e?.kind ?? '')}</td>
                  <td className="muted">
                    {hex16(e?.profile_id)}
                    {describeProfile(e?.profile_id)?.name ? ` (${describeProfile(e?.profile_id).name})` : ''}
                  </td>
                  <td className="muted">
                    {hex16(e?.device_id)}
                    {describeDeviceId(e?.device_id)?.name ? ` (${describeDeviceId(e?.device_id).name})` : ''}
                  </td>
                  <td>
                    <div className="row" style={{ flexWrap: 'wrap', gap: 8 }}>
                      {hasAccept(e, 'onoff.') ? (
                        <>
                          <label className="muted" style={{ display: 'inline-flex', alignItems: 'center', gap: 6 }}>
                            <input
                              type="checkbox"
                              checked={Boolean(state?.onoff)}
                              onChange={(ev) => sendOnOff(Number(e?.endpoint ?? 1), ev?.target?.checked ? 'on' : 'off')}
                            />
                            On
                          </label>
                          <button onClick={() => sendOnOff(Number(e?.endpoint ?? 1), 'toggle')}>Toggle</button>
                        </>
                      ) : null}

                      {hasAccept(e, 'level.move_to_level') || hasAccept(e, 'level.') ? (
                        <>
                          <input
                            type="range"
                            min={0}
                            max={254}
                            value={Number(levelByEndpoint.get(Number(e?.endpoint ?? 0)) ?? 0)}
                            onChange={(ev) => {
                              const v = Number(ev?.target?.value ?? 0)
                              setLevelByEndpoint((prev) => {
                                const next = new Map(prev)
                                next.set(Number(e?.endpoint ?? 0), v)
                                return next
                              })
                            }}
                          />
                          <button onClick={() => sendLevel(Number(e?.endpoint ?? 1))}>Set</button>
                        </>
                      ) : null}

                      {hasAccept(e, 'color.move_to_color_xy') || hasAccept(e, 'color.') ? (
                        <>
                          <input
                            type="color"
                            value={String(colorByEndpoint.get(Number(e?.endpoint ?? 0)) ?? '#ffffff')}
                            onChange={(ev) => {
                              const v = String(ev?.target?.value ?? '#ffffff')
                              setColorByEndpoint((prev) => {
                                const next = new Map(prev)
                                next.set(Number(e?.endpoint ?? 0), v)
                                return next
                              })
                            }}
                            title="Color (xy)"
                          />
                          <button onClick={() => sendColor(Number(e?.endpoint ?? 1))}>Set</button>
                        </>
                      ) : null}

                      {hasAccept(e, 'color.move_to_color_temperature') ? (
                        <>
                          <input
                            type="range"
                            min={2000}
                            max={6500}
                            step={100}
                            value={Number(tempKByEndpoint.get(Number(e?.endpoint ?? 0)) ?? 3000)}
                            onChange={(ev) => {
                              const v = Number(ev?.target?.value ?? 3000)
                              setTempKByEndpoint((prev) => {
                                const next = new Map(prev)
                                next.set(Number(e?.endpoint ?? 0), v)
                                return next
                              })
                            }}
                            title="Color temperature (K)"
                          />
                          <button onClick={() => sendTemp(Number(e?.endpoint ?? 1))}>Set</button>
                        </>
                      ) : null}
                    </div>
                  </td>
                  <td className="mono">{listToText(e?.accepts)}</td>
                  <td className="mono">{listToText(e?.emits)}</td>
                  <td className="mono">{listToText(e?.reports)}</td>
                  <td className="mono">
                    {Array.isArray(e?.in_clusters)
                      ? e.in_clusters
                          .map((c) => `${hex16(c)}${describeCluster(c)?.name ? ` ${describeCluster(c).name}` : ''}`)
                          .join(', ')
                      : ''}
                  </td>
                  <td className="mono">
                    {Array.isArray(e?.out_clusters)
                      ? e.out_clusters
                          .map((c) => `${hex16(c)}${describeCluster(c)?.name ? ` ${describeCluster(c).name}` : ''}`)
                          .join(', ')
                      : ''}
                  </td>
                </tr>
              ))
            )}
          </tbody>
        </table>
      </div>

      <div style={{ height: 12 }} />

      <div className="card">
        <table>
          <thead>
            <tr>
              <th>Endpoint</th>
              <th>Cluster</th>
              <th>Attr</th>
              <th>Value</th>
              <th>ts_ms</th>
            </tr>
          </thead>
          <tbody>
            {sortedSensors.length === 0 ? (
              <tr>
                <td colSpan={5} className="muted">
                  No sensor values yet (wait for reports or initial reads).
                </td>
              </tr>
            ) : (
              sortedSensors.map((s, idx) => (
                <tr key={`${idx}-${s?.endpoint}-${s?.cluster_id}-${s?.attr_id}`}>
                  <td>
                    <code>{String(s?.endpoint ?? '')}</code>
                  </td>
                  <td className="muted">
                    {hex16(s?.cluster_id)}
                    {describeCluster(s?.cluster_id)?.name ? ` (${describeCluster(s?.cluster_id).name})` : ''}
                  </td>
                  <td className="muted">
                    {hex16(s?.attr_id)}
                    {describeAttr(s?.cluster_id, s?.attr_id)?.name ? ` (${describeAttr(s?.cluster_id, s?.attr_id).name})` : ''}
                  </td>
                  <td className="mono">{renderSensorValue(s)}</td>
                  <td className="muted">{String(s?.ts_ms ?? '')}</td>
                </tr>
              ))
            )}
          </tbody>
        </table>
      </div>
    </div>
  )
}
