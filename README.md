# Iot-grain-dryer-controller

IoT system to **optimize corn drying** in a small farm with photovoltaics: it forecasts PV generation and loads, decides **furnace ON/OFF** to reduce peaks and consumption, and enforces a **minimum daily runtime** (anti-starvation). Data and predictions are stored in **MySQL** and visualized with **Grafana**.

## Architecture (at a glance)

- **6 Contiki-NG nodes over 6LoWPAN**: 2 sensors (Roof, Power), 1 Edge (decisions), 2 actuators (Furnace, Alarm), 1 Border Router. **CoAP** communication with **JSON** payloads.  
- **On-device ML**:  
  - *Roof*: MLP (Emlearn/TFLM) for **next_solar**.  
  - *Edge*: MLP for **next_power/difference** and decision logic. Quantized models, a few hundred KB, real-time inference.  
- **CoAPthon server**: resources `/register`, `/lookup`, `/res_data`, `/res_prediction`, `/starvation`; **MySQL** integration; remote **observe** of `/res_furnace` and `/res_threshold`.  
- **Anti-starvation**: 24-hour sliding window with `min_on`/`max_on` and `load_hour`, including forced activations and (if needed) disabling edge auto-control.  
- **Dashboard**: PV vs forecast, consumption vs forecast, weather, furnace duty cycle, and ON/OFF state.

## Screenshots / Diagram

> *(Insert an architecture diagram or Grafana screenshots.)*

---

## Key features

- **Smart furnace activation** using thresholds and forecasts (Alarm LED blue/green/red/purple to reflect energy state).  
- **Time-series persistence**: measurements, predictions, and furnace state with `time_sec` timestamps for Grafana.  
- **Control CLI**: manual overrides, thresholds, Auto toggle, min/max daily runtime and load hour.

---

## Requirements

- **Firmware**: Contiki-NG (sensor/actuator/edge nodes) and tunslip border router.  
- **Server**: Python 3.x with **CoAPthon**, MySQL (schema below), Grafana.

> Pin exact versions in `requirements.txt` / `pyproject.toml` and document firmware build steps.

---

## Quick start

1) **Deploy the IoT network**  
Flash the 6 nodes (Roof, Power, Edge, Furnace, Alarm, Border Router). On boot, nodes find the root, **register resources** on the server, and start periodic PUTs to the Edge.

2) **Run the CoAPthon server**  
The server binds to `::`:5683, exposes resources, starts **observe** threads on `/res_furnace` and `/res_threshold`, then `listen(10)`.

3) **Configure DB & Grafana**  
Tables are created on first resource access. Grafana reads the series and renders PV, consumption, weather, duty cycle, and state widgets.

---

## CoAP API (server)

- `POST /register`  
  Node self-registration: stores `node_ip` and resources; `GET /register` returns epoch for time sync.

- `POST /res_data`  
  Receives Edge-aggregated metrics (solar, power, temp, hum), converts timestamp to `time_sec`, inserts into `res_data`, then calls `avoid_starvation()`.

- `POST /res_prediction`  
  Receives `next_power`, `next_solar`, `missing`, stores in `res_prediction`.

- `GET /lookup?res=/resource_name`  
  Returns `{ "ip": "<addr>" }` if found, else 4.04.

- `GET|PUT /starvation`  
  Read/update `min_on`, `max_on`, `load_hour` (immediate effect).

> **Note**: the server keeps a **15s furnace state logger** in `furnace_log` for Grafana.

---

## Edge logic (local decisions)

After receiving measurements and `next_solar`, the Edge computes `next_power` and decides:  
- If `next_solar − next_power < threshold_on` ⇒ optimal conditions: **turn ON** furnace (Auto) and **blue Alarm**.  
- Between `threshold_on` and `threshold_off` ⇒ keep state, **green Alarm**.  
- If `> threshold_off` ⇒ **turn OFF**, **red Alarm**.  
- If `> threshold_cut` ⇒ buzzer + **purple Alarm**, **forced shutdown**.

The Edge exposes `/res_threshold` with `threshold_on`, `threshold_off`, and `auto_furnace_ctrl` (also toggled via button).

---

## CLI (main commands)

1. **Turn furnace on/off** (force)  
2. **Set thresholds** ON/OFF (W)  
3. **Enable/Disable Auto Control**  
4. **Set max/min daily runtime** (h/24h)  
5. **Set load time** (hour of day)  
6. **System Info** (state, mode, thresholds, min/max, hours)

---

## Data schema (MySQL)

- **nodes**: registry for resource/IP discovery (multiple resources per node).  
- **res_data**: measurements (temp, hum, total power, solar, timestamp and `time_sec`).  
- **res_prediction**: predictions `next_solar`, `next_power`, linked via `time_sec`.  
- **furnace_log**: ON/OFF state with `time_sec`. *(PK: `id` in all tables).*

---

## Anti-Starvation (minimum hours guarantee)

Maintains a 24-hour sliding window of furnace runtime: ensures **≥ `min_on`** hours and **≤ `max_on`** before `load_hour`. If needed it:  
- issues PUT to **disable Auto** on the Edge,  
- **forces ON/OFF** to schedule remaining hours,  
- keeps Auto disabled if the maximum is exceeded until reset.

---

## Dashboard (Grafana)

- **Solar**: actual PV vs **+1h forecast**.  
- **Power**: actual consumption vs **forecast**.  
- **Weather**: temperature/humidity.  
- **Use Furnace**: duty-cycle pie.  
- **State Furnace**: binary ON/OFF timeline.

---

## Design choices

- **Protocol: CoAP** (no broker, lower overhead, native in Contiki-NG, observe).  
- **Format: JSON** (unified across sensors, actuators, CLI; lighter than XML).

---

## Suggested repo structure

