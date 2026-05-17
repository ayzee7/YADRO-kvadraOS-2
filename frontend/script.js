// Color helper: green -> yellow -> red based on percentage
function pctColor(pct) {
  if (pct < 60) return "var(--green)";
  if (pct < 85) return "var(--yellow)";
  return "var(--red)";
}

// Format uptime as h:mm:ss
function formatUptime(secs) {
  const h = Math.floor(secs / 3600);
  const m = Math.floor((secs % 3600) / 60);
  const s = secs % 60;
  return `${h}:${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")}`;
}

// Format CPU time as m:ss.xx
function formatCPUTime(csecs) {
  const m = Math.floor(csecs / 6000);
  const s = Math.floor((csecs % 6000) / 100);
  const hs = csecs % 100;
  return `${m}:${String(s).padStart(2, "0")}.${String(hs).padStart(2, "0")}`;
}

// Set a progress bar + color
function setBar(barId, pct) {
  const bar = document.getElementById(barId);
  bar.style.width = Math.min(pct, 100) + "%";
  bar.style.background = pctColor(pct);
}

// Render stats cards
function renderStats(data) {
  // CPU
  const cpuPct = Math.round(data.cpu_percent * 10) / 10;
  document.getElementById("cpu-pct").textContent = cpuPct + "%";
  document.getElementById("cpu-pct").style.color = pctColor(cpuPct);
  document.getElementById("cpu-detail").textContent =
    cpuPct.toFixed(1) + "% used";
  setBar("cpu-bar", cpuPct);

  // RAM
  const memPct = Math.round((data.mem_used_kb / data.mem_total_kb) * 1000) / 10;
  const memUsedMib = (data.mem_used_kb / 1024).toFixed(0);
  const memTotalMib = (data.mem_total_kb / 1024).toFixed(0);
  document.getElementById("mem-pct").textContent = memPct + "%";
  document.getElementById("mem-pct").style.color = pctColor(memPct);
  document.getElementById("mem-detail").textContent =
    `${memUsedMib} / ${memTotalMib} MiB`;
  setBar("mem-bar", memPct);

  // Swap
  if (data.swap_total_kb > 0) {
    const swapPct =
      Math.round((data.swap_used_kb / data.swap_total_kb) * 1000) / 10;
    const swapUsedMib = (data.swap_used_kb / 1024).toFixed(0);
    const swapTotalMib = (data.swap_total_kb / 1024).toFixed(0);
    document.getElementById("swap-pct").textContent = swapPct + "%";
    document.getElementById("swap-pct").style.color = pctColor(swapPct);
    document.getElementById("swap-detail").textContent =
      `${swapUsedMib} / ${swapTotalMib} MiB`;
    setBar("swap-bar", swapPct);
  } else {
    document.getElementById("swap-pct").textContent = "N/A";
    document.getElementById("swap-detail").textContent = "no swap configured";
  }
}

// Sort state
let sortCol = "cpu";
let sortAsc = false;

// Render process table
function renderProcesses(processes) {
  const sorted = [...processes].sort((a, b) => {
    const av = a[sortCol];
    const bv = b[sortCol];
    const cmp =
      typeof av === "number" ? av - bv : String(av).localeCompare(String(bv));
    return sortAsc ? cmp : -cmp;
  });

  const tbody = document.getElementById("proc-tbody");
  tbody.innerHTML = "";

  sorted.forEach((p) => {
    const tr = document.createElement("tr");
    tr.innerHTML = `
        <td class="col-pid">${p.pid}</td>
        <td class="col-user">${p.user}</td>
        <td class="col-priority">${p.priority}</td>
        <td class="col-nice">${p.nice}</td>
        <td class="col-virt">${p.virt}</td>
        <td class="col-res">${p.res}</td>
        <td class="col-state">${p.state}</td>
        <td class="col-cpu" style="color:${pctColor(p.cpu)}">${p.cpu.toFixed(1)}%</td>
        <td class="col-mem" style="color:${pctColor(p.mem)}">${p.mem.toFixed(1)}%</td>
        <td class="col-time">${formatCPUTime(p.time)}</td>
        <td class="col-cmd" title="${p.command}">${p.command}</td>
        `;
    tbody.appendChild(tr);
  });
}

// Save table locally for sort to act instantly
let lastProcesses = [];

// Column sort on header click
document.querySelectorAll("thead th[data-col]").forEach((th) => {
  th.addEventListener("click", () => {
    const col = th.dataset.col;
    if (sortCol === col) {
      sortAsc = !sortAsc;
    } else {
      sortCol = col;
      sortAsc = col === "pid" || col === "user" || col === "command";
    }
    renderProcesses(lastProcesses);
  });
});

// Poll the backend every 2 seconds
async function fetchStats() {
  try {
    const resp = await fetch("/api/stats");
    if (!resp.ok) throw new Error("HTTP " + resp.status);
    const data = await resp.json();

    renderStats(data);
    lastProcesses = data.processes;
    renderProcesses(data.processes);

    document.getElementById("status").textContent =
      "last update: " + new Date().toLocaleTimeString();
    document.getElementById("uptime").textContent =
      `uptime: ${formatUptime(data.uptime)}`;
  } catch (err) {
    document.getElementById("status").textContent = "error: " + err.message;
  }
}

fetchStats();
setInterval(fetchStats, 2000);
