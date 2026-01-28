export function hex16(v) {
  const n = Number(v ?? 0)
  if (!Number.isFinite(n)) return ''
  const x = Math.max(0, Math.min(0xffff, n)) >>> 0
  return `0x${x.toString(16).padStart(4, '0')}`
}

export function describeProfile(profileId) {
  const id = Number(profileId ?? NaN)
  if (!Number.isFinite(id)) return null
  if (id === 0x0104) return { name: 'Home Automation' }
  return null
}

export function describeDeviceId(deviceId) {
  const id = Number(deviceId ?? NaN)
  if (!Number.isFinite(id)) return null

  // Minimal set for now; extend as needed.
  if (id === 0x0302) return { name: 'Temperature Sensor' }
  return null
}

export function describeCluster(clusterId) {
  const id = Number(clusterId ?? NaN)
  if (!Number.isFinite(id)) return null

  // Common ZCL cluster IDs.
  switch (id) {
    case 0x0000:
      return { name: 'Basic' }
    case 0x0001:
      return { name: 'Power Configuration' }
    case 0x0003:
      return { name: 'Identify' }
    case 0x0004:
      return { name: 'Groups' }
    case 0x0005:
      return { name: 'Scenes' }
    case 0x0006:
      return { name: 'On/Off' }
    case 0x0008:
      return { name: 'Level Control' }
    case 0x0300:
      return { name: 'Color Control' }
    case 0x0400:
      return { name: 'Illuminance Measurement' }
    case 0x0402:
      return { name: 'Temperature Measurement' }
    case 0x0403:
      return { name: 'Pressure Measurement' }
    case 0x0405:
      return { name: 'Relative Humidity Measurement' }
    case 0x0406:
      return { name: 'Occupancy Sensing' }
    case 0x0500:
      return { name: 'IAS Zone' }
    case 0x0702:
      return { name: 'Metering' }
    case 0x0b04:
      return { name: 'Electrical Measurement' }
    case 0x0021:
      return { name: 'Green Power' }
    default:
      return null
  }
}

export function describeAttr(clusterId, attrId) {
  const c = Number(clusterId ?? NaN)
  const a = Number(attrId ?? NaN)
  if (!Number.isFinite(c) || !Number.isFinite(a)) return null

  // A small set of useful attributes; extend incrementally.
  if (c === 0x0006 && a === 0x0000) return { name: 'OnOff', unit: null }

  // Temperature Measurement: MeasuredValue is int16 in 0.01°C.
  if (c === 0x0402 && a === 0x0000) return { name: 'MeasuredValue', unit: '°C', scale: 0.01, signed: true }

  // Relative Humidity Measurement: MeasuredValue is uint16 in 0.01%.
  if (c === 0x0405 && a === 0x0000) return { name: 'MeasuredValue', unit: '%', scale: 0.01, signed: false }

  // Power Configuration: BatteryPercentageRemaining is uint8 in 0.5%.
  if (c === 0x0001 && a === 0x0021) return { name: 'BatteryPercentageRemaining', unit: '%', scale: 0.5, signed: false }
  if (c === 0x0001 && a === 0x0020) return { name: 'BatteryVoltage', unit: 'V', scale: 0.1, signed: false }

  // Illuminance Measurement: MeasuredValue is uint16 (logarithmic lux in spec).
  if (c === 0x0400 && a === 0x0000) return { name: 'MeasuredValue', unit: null }

  return null
}

export function formatSensorValue(sensorRow) {
  if (!sensorRow) return ''
  const c = Number(sensorRow.cluster_id ?? NaN)
  const a = Number(sensorRow.attr_id ?? NaN)
  const info = describeAttr(c, a)

  const raw =
    typeof sensorRow.value_i32 === 'number'
      ? sensorRow.value_i32
      : typeof sensorRow.value_u32 === 'number'
        ? sensorRow.value_u32
        : null

  if (raw == null || !Number.isFinite(raw)) return ''

  if (info?.scale && typeof info.scale === 'number') {
    const v = raw * info.scale
    const digits = info.scale < 1 ? Math.min(3, Math.max(0, Math.round(-Math.log10(info.scale)))) : 0
    const unit = info.unit ? ` ${info.unit}` : ''
    return `${v.toFixed(digits)}${unit} (raw ${raw})`
  }

  return info?.unit ? `${raw} ${info.unit}` : `${raw}`
}

