import { Navigate, NavLink, Route, Routes } from 'react-router-dom'
import './App.css'
import Devices from './pages/Devices.jsx'

function App() {
  return (
    <div className="layout">
      <nav className="nav">
        <div className="brand">Zigbee Gateway</div>
        <NavLink to="/devices" className={({ isActive }) => (isActive ? 'navlink active' : 'navlink')}>
          Devices
        </NavLink>
      </nav>

      <main className="content">
        <Routes>
          <Route path="/" element={<Navigate to="/devices" replace />} />
          <Route path="/devices" element={<Devices />} />
          <Route path="*" element={<Navigate to="/devices" replace />} />
        </Routes>
      </main>
    </div>
  )
}

export default App
