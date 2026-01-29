import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

function exists(p) {
	try {
		fs.accessSync(p)
		return true
	} catch {
		return false
	}
}

function listDirs(p) {
	try {
		return fs
			.readdirSync(p, { withFileTypes: true })
			.filter((d) => d.isDirectory())
			.map((d) => d.name)
	} catch {
		return []
	}
}

function pickIdfPath() {
	if (process.env.IDF_PATH && exists(path.join(process.env.IDF_PATH, 'tools', 'idf.py'))) {
		return process.env.IDF_PATH
	}

	const home = os.homedir()
	const root = path.join(home, 'esp-idf')

	// Common: ~/esp-idf/vX.Y.Z/esp-idf
	for (const v of listDirs(root).filter((n) => n.startsWith('v')).sort().reverse()) {
		const candidate = path.join(root, v, 'esp-idf')
		if (exists(path.join(candidate, 'tools', 'idf.py'))) {
			return candidate
		}
	}

	// Fallback: ~/esp-idf/esp-idf
	{
		const candidate = path.join(root, 'esp-idf')
		if (exists(path.join(candidate, 'tools', 'idf.py'))) {
			return candidate
		}
	}

	throw new Error('IDF_PATH not found. Set IDF_PATH env var to your ESP-IDF directory (the one that contains tools/idf.py).')
}

function pickToolsPath() {
	if (process.env.IDF_TOOLS_PATH && exists(process.env.IDF_TOOLS_PATH)) {
		return process.env.IDF_TOOLS_PATH
	}

	const home = os.homedir()
	const candidates = [path.join(home, 'esp-idf', '.espressif'), path.join(home, '.espressif')]
	for (const c of candidates) {
		if (exists(c)) return c
	}

	throw new Error('IDF_TOOLS_PATH not found. Set IDF_TOOLS_PATH env var to your .espressif directory.')
}

function pickPythonExe(toolsPath) {
	// Prefer IDF virtualenv if present.
	const pyEnvRoot = path.join(toolsPath, 'python_env')
	const envs = listDirs(pyEnvRoot).sort().reverse()
	for (const envName of envs) {
		const py = path.join(pyEnvRoot, envName, 'Scripts', 'python.exe')
		if (exists(py)) return py
	}

	// Fallback to bundled idf-python
	const idfPyRoot = path.join(toolsPath, 'tools', 'idf-python')
	for (const ver of listDirs(idfPyRoot).sort().reverse()) {
		const py = path.join(idfPyRoot, ver, 'python.exe')
		if (exists(py)) return py
	}

	throw new Error('Python for ESP-IDF not found under IDF_TOOLS_PATH. Run ESP-IDF tools installer or idf_tools.py install-python-env.')
}

function idfExportEnv(pythonExe, idfPath, toolsPath) {
	const res = spawnSync(pythonExe, [path.join(idfPath, 'tools', 'idf_tools.py'), '--idf-path', idfPath, 'export', '--format', 'key-value'], {
		encoding: 'utf8',
		env: {
			...process.env,
			IDF_PATH: idfPath,
			IDF_TOOLS_PATH: toolsPath,
		},
	})
	if (res.status !== 0) {
		throw new Error(`idf_tools.py export failed:\n${res.stderr || res.stdout || ''}`)
	}

	const envOut = { ...process.env, IDF_PATH: idfPath, IDF_TOOLS_PATH: toolsPath }
	for (const line of String(res.stdout || '').split(/\r?\n/)) {
		if (!line) continue
		const idx = line.indexOf('=')
		if (idx <= 0) continue
		const key = line.slice(0, idx)
		let value = line.slice(idx + 1)
		if (key === 'PATH') {
			value = value.replace('%PATH%', process.env.PATH || '')
		}
		envOut[key] = value
	}
	return envOut
}

function main() {
	const __filename = fileURLToPath(import.meta.url)
	const __dirname = path.dirname(__filename)
	const webDir = path.resolve(__dirname, '..')
	const projectDir = path.resolve(webDir, '..')

	const idfPath = pickIdfPath()
	const toolsPath = pickToolsPath()
	const pythonExe = pickPythonExe(toolsPath)
	const env = idfExportEnv(pythonExe, idfPath, toolsPath)

	const passthrough = process.argv.slice(2).map((a) => (a === '-h' ? '--help' : a))
	const wantHelp = passthrough.includes('--help')
	const globalArgs = passthrough.filter((a) => a !== '--help')

	// Enforce explicit port to avoid flashing a different ESP if multiple devices are connected.
	let port = null
	for (let i = 0; i < globalArgs.length; i++) {
		const a = globalArgs[i]
		if (a === '-p' || a === '--port') {
			port = globalArgs[i + 1] || null
			i += 1
			continue
		}
		if (a.startsWith('--port=')) {
			port = a.slice('--port='.length) || null
		}
	}
	if (!port && !wantHelp) {
		console.error('Error: port is required. Use: npm run esp -- -p COM5')
		process.exit(2)
	}

	// Note: idf.py global options (like -p/--port) must come before the target.
	// Keep --help after the target to show target-specific help.
	const args = [
		path.join(idfPath, 'tools', 'idf.py'),
		'-C',
		projectDir,
		...globalArgs,
		'www-flash',
		...(wantHelp ? ['--help'] : []),
	]

	const res = spawnSync(pythonExe, args, { stdio: 'inherit', env })
	process.exit(res.status ?? 1)
}

main()
