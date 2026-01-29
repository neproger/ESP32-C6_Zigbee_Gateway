import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react'

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

function wsUrl(path) {
	const proto = window.location.protocol === 'https:' ? 'wss' : 'ws'
	return `${proto}://${window.location.host}${path}`
}

export default function Events() {
	const [events, setEvents] = useState([])
	const [lastId, setLastId] = useState(0)
	const [paused, setPaused] = useState(false)
	const [showMsg, setShowMsg] = useState(false)
	const [deviceNameByUid, setDeviceNameByUid] = useState(() => new Map())
	const [status, setStatus] = useState('')
	const [conn, setConn] = useState('disconnected')
	const [flashIds, setFlashIds] = useState(() => new Set())

	const lastIdRef = useRef(0)
	const wsRef = useRef(null)
	const reconnectTimerRef = useRef(null)
	const scrollRef = useRef(null)
	const serverLastIdRef = useRef(null)
	const catchingUpRef = useRef(true)
	const pinnedToBottomRef = useRef(true)

	const isNearBottom = (el) => {
		if (!el) return false
		const thresholdPx = 48
		const gap = el.scrollHeight - el.scrollTop - el.clientHeight
		return gap <= thresholdPx
	}

	useEffect(() => {
		load()
	}, [])

	const loadDeviceNames = useCallback(async () => {
		try {
			const r = await fetch('/api/devices')
			if (!r.ok) throw new Error(`GET /api/devices failed: ${r.status}`)
			const data = await r.json()
			const items = Array.isArray(data) ? data : []
			setDeviceNameByUid(() => {
				const next = new Map()
				for (const d of items) {
					const uid = String(d?.device_uid ?? '')
					const name = String(d?.name ?? '')
					if (uid) next.set(uid, name)
				}
				return next
			})
		} catch {
			// ignore
		}
	}, [])

	useEffect(() => {
		loadDeviceNames()
	}, [loadDeviceNames])

	useEffect(() => {
		if (!Array.isArray(events) || events.length === 0) return
		for (const e of events) {
			const uid = String(e?.device_uid ?? '')
			if (uid && !deviceNameByUid.has(uid)) {
				loadDeviceNames()
				break
			}
		}
	}, [events, deviceNameByUid, loadDeviceNames])

	useEffect(() => {
		const cleanup = () => {
			if (reconnectTimerRef.current) {
				clearTimeout(reconnectTimerRef.current)
				reconnectTimerRef.current = null
			}
			if (wsRef.current) {
				try {
					wsRef.current.close()
				} catch { }
				wsRef.current = null
			}
			setConn('disconnected')
		}

		if (paused) {
			cleanup()
			return
		}

		let attempts = 0

		const connect = () => {
			cleanup()
			setConn('connecting')

			const ws = new WebSocket(wsUrl('/ws'))
			wsRef.current = ws

			ws.onopen = () => {
				attempts = 0
				setConn('connected')
				setStatus('')
				serverLastIdRef.current = null
				catchingUpRef.current = true
				const since = lastIdRef.current ?? 0
				ws.send(JSON.stringify({ t: 'hello', proto: 'gw-ws-1', subs: ['events'], since }))
			}

			ws.onmessage = (ev) => {
				try {
					const msg = JSON.parse(String(ev?.data ?? ''))
					if (msg?.t === 'hello') {
						const last = Number(msg?.event_last_id ?? 0)
						serverLastIdRef.current = Number.isFinite(last) ? last : null
						if (serverLastIdRef.current == null) {
							catchingUpRef.current = false
						}
						return
					}
					if (msg?.t === 'event') {
						// Capture pinned state before DOM grows (otherwise "gap" increases and we won't auto-scroll).
						pinnedToBottomRef.current = isNearBottom(scrollRef.current)

						const id = Number(msg?.id ?? 0)
						if (Number.isFinite(id) && id > (lastIdRef.current ?? 0)) {
							lastIdRef.current = id
							setLastId(id)
						}

						// Don't "flash" the initial backlog replay; only flash truly new events after catch-up.
						const serverLast = serverLastIdRef.current
						const isCatchingUp = catchingUpRef.current === true
						const shouldFlash =
							!isCatchingUp ||
							(serverLast != null && Number.isFinite(serverLast) && id >= serverLast)
						if (serverLast != null && Number.isFinite(serverLast) && id >= serverLast) {
							catchingUpRef.current = false
						}

						if (shouldFlash) {
							setFlashIds((prev) => {
								const next = new Set(prev)
								next.add(id)
								return next
							})
							setTimeout(() => {
								setFlashIds((prev) => {
									if (!prev.has(id)) return prev
									const next = new Set(prev)
									next.delete(id)
									return next
								})
							}, 4800)
						}

						setEvents((prev) => {
							const merged = [...(Array.isArray(prev) ? prev : []), msg]
							const max = 200
							return merged.length > max ? merged.slice(merged.length - max) : merged
						})
					}
				} catch (e) {
					// ignore parse errors
				}
			}

			ws.onclose = () => {
				setConn('disconnected')
				attempts += 1
				const delay = Math.min(5000, 250 * 2 ** Math.min(attempts, 5))
				reconnectTimerRef.current = setTimeout(connect, delay)
			}

			ws.onerror = () => {
				// onclose will follow; keep errors in status only if we never connected
				setStatus((s) => s || 'WebSocket error')
			}
		}

		connect()
		return cleanup
	}, [paused])

	useEffect(() => {
		const el = scrollRef.current
		if (!el) return

		const onScroll = () => {
			pinnedToBottomRef.current = isNearBottom(el)
		}

		el.addEventListener('scroll', onScroll, { passive: true })
		onScroll()
		return () => el.removeEventListener('scroll', onScroll)
	}, [paused])

	useLayoutEffect(() => {
		if (paused) return
		const el = scrollRef.current
		if (!el) return
		if (!pinnedToBottomRef.current) return
		el.scrollTop = el.scrollHeight
	}, [events, paused])

	const sortedEvents = useMemo(() => {
		return [...events].sort(
			(a, b) => Number(a?.id ?? 0) - Number(b?.id ?? 0)
		)
	}, [events])

	const clear = useCallback(() => {
		setEvents([])
		lastIdRef.current = 0
		setLastId(0)
		setFlashIds(new Set())
	}, [])

	const payloadToText = useCallback((payload) => {
		if (payload == null) return ''
		try {
			return JSON.stringify(payload, null, 2)
		} catch {
			return String(payload)
		}
	}, [])

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
					return incoming.length > max ? incoming.slice(incoming.length - max) : incoming
				})
			}

			setStatus('')
		} catch (e) {
			setStatus(String(e?.message ?? e))
		}
	}, [])

	return (
		<div className="page">
			<div className="header">
				<div>
					<h1>Events</h1>
					<div className="muted">In-memory gateway event log (WebSocket /ws).</div>
				</div>
				<div className="row">
					<button onClick={() => setPaused((p) => !p)}>{paused ? 'Resume' : 'Pause'}</button>
					<button onClick={clear}>Clear</button>
					<label className="muted" style={{ display: 'inline-flex', gap: 8, alignItems: 'center' }}>
						<input type="checkbox" checked={showMsg} onChange={(e) => setShowMsg(Boolean(e?.target?.checked))} />
						Show msg
					</label>
					<button onClick={load} disabled={paused}>
						Refresh
					</button>
					<div className="muted">ws: {conn}</div>
					<div className="muted">last_id: {lastId}</div>
				</div>
			</div>

			{status ? <div className="status">{status}</div> : null}

			<div ref={scrollRef} className="card scroll height-100">
				<table >
					<thead>
						<tr>
									<th>ID</th>
									<th>Time</th>
									<th>Type</th>
									<th>Source</th>
									<th>Device</th>
									<th>Short</th>
									<th>Payload</th>
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
							sortedEvents.map((e, index) => (
								<tr
									key={String(e?.id ?? index)}
									className={flashIds.has(Number(e?.id ?? -1)) ? 'event-flash' : ''}
								>
									<td>
										<code>{String(e?.id ?? '')}</code>
									</td>
									<td className="muted">{msToUptime(e?.ts_ms)}</td>
									<td>
										{String(e?.type ?? '')}
										{e?.v != null ? <span className="muted"> v{String(e?.v)}</span> : null}
									</td>
									<td className="muted">{String(e?.source ?? '')}</td>
									<td>
										{(() => {
											const uid = String(e?.device_uid ?? '')
											const name = uid ? deviceNameByUid.get(uid) : ''
											const label = (name && name !== 'undefined' && name !== 'null') ? name : uid
											return label ? <span title={uid}>{label}</span> : <span className="muted">-</span>
										})()}
									</td>
									<td>
										<code>{shortToHex(e?.short_addr)}</code>
									</td>
									<td className="mono" style={{ whiteSpace: 'pre-wrap' }}>
										{e?.payload != null ? payloadToText(e?.payload) : <span className="muted">no payload</span>}
										{showMsg && e?.msg ? (
											<div className="muted mono" style={{ marginTop: 8, whiteSpace: 'pre-wrap' }}>
												msg: {String(e?.msg ?? '')}
											</div>
										) : null}
									</td>
								</tr>
							))
						)}
					</tbody>
				</table>
			</div>
		</div>
	)
}
