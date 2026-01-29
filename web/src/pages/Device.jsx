import { Link, useParams } from 'react-router-dom'
import { useCallback, useEffect, useMemo, useState } from 'react'
import { describeAttr, describeCluster, describeDeviceId, describeProfile, formatSensorValue, hex16 } from '../zigbee/zcl.js'

function renderSensorValue(s) {
  return formatSensorValue(s)
}

function listToText(v) {
  return Array.isArray(v) ? v.map((x) => String(x ?? '')).filter(Boolean).join(', ') : ''
}

export default function Device() {
  const { uid } = useParams()
  const [device, setDevice] = useState(null)
  const [endpoints, setEndpoints] = useState([])
  const [sensors, setSensors] = useState([])
  const [status, setStatus] = useState('')

  const load = useCallback(async () => {
    const u = String(uid ?? '')
    if (!u) return
    setStatus('')
    try {
      const [dr, epr, sr] = await Promise.all([
        fetch(`/api/devices`),
        fetch(`/api/endpoints?uid=${encodeURIComponent(u)}`),
        fetch(`/api/sensors?uid=${encodeURIComponent(u)}`),
      ])
      if (!dr.ok) throw new Error(`GET /api/devices failed: ${dr.status}`)
      if (!epr.ok) throw new Error(`GET /api/endpoints failed: ${epr.status}`)
      if (!sr.ok) throw new Error(`GET /api/sensors failed: ${sr.status}`)
      const devs = await dr.json()
      const d = Array.isArray(devs) ? devs.find((x) => String(x?.device_uid ?? '') === u) : null
      setDevice(d ?? null)
      const epData = await epr.json()
      const sData = await sr.json()
      setEndpoints(Array.isArray(epData) ? epData : [])
      setSensors(Array.isArray(sData) ? sData : [])
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
                <td colSpan={9} className="muted">
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
