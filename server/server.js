const express = require('express');
const app = express();

app.use(express.json()); // Built-in middleware to parse JSON
app.use(express.urlencoded({ extended: true })); // Built-in middleware to parse URL-encoded forms

// 1. Create a data store for multiple rooms instead of single global variables
const defaultRoom = () => ({
  temperature: 0, humidity: 0, pressure: 0,
  IAQ: 0, carbon: 0, VOC: 0, IAQsts: "Unknown",
  lux: 0, sound: 0, soundPeak: 0, soundDb: 0, battery: 0,
  rssi: null, wifiPercent: null   // null = firmware never reported WiFi quality
});

// Rooms are created on the fly from the `location` tag each node POSTs, so a
// new node (e.g. "Hallway") appears on the dashboard without a code change.
let roomData = {};

// Endpoint to receive sensor data from ESP32s
app.post('/sensor-data', (req, res) => {
  // 2. Extract the 'location' tag you added in your Arduino sketches
  const {
    location, temperature, humidity, pressure,
    IAQ, carbon, VOC, IAQsts,
    lux, sound, soundPeak, soundDb, battery,
    rssi, wifiPercent
  } = req.body;

  // Default to "Unknown" if no location is sent
  const loc = location || "Unknown";

  // If this is a brand new room we haven't seen before, initialize it
  if (!roomData[loc]) {
      roomData[loc] = defaultRoom();
  }

  // Update the variables ONLY for the specific room that sent the request
  roomData[loc].temperature = temperature;
  roomData[loc].humidity = humidity;
  roomData[loc].pressure = pressure;
  roomData[loc].IAQ = IAQ;
  roomData[loc].carbon = carbon;
  roomData[loc].VOC = VOC;
  roomData[loc].IAQsts = IAQsts;
  if (lux !== undefined) roomData[loc].lux = lux;
  if (sound !== undefined) roomData[loc].sound = sound;
  if (soundPeak !== undefined) roomData[loc].soundPeak = soundPeak;
  if (soundDb !== undefined) roomData[loc].soundDb = soundDb;
  if (battery !== undefined) roomData[loc].battery = battery;
  if (rssi !== undefined) roomData[loc].rssi = rssi;
  if (wifiPercent !== undefined) roomData[loc].wifiPercent = wifiPercent;

  console.log(`[INFO] Received data from ${loc}:`, req.body);
  res.status(200).send('Data received');
});

// Serve the HTML page
app.get('/', (req, res) => {
  res.send(SendHTML());
});

// Endpoint to provide sensor data as JSON
app.get('/data', (req, res) => {
  // 3. Return the entire object containing all rooms
  res.json(roomData); 
});

function SendHTML() {
  // I have rewritten this using Template Literals (backticks) for easier editing.
  // It also includes a flexbox layout to display the rooms side-by-side on large screens!
  return `
    <!DOCTYPE html>
    <html>
    <head>
        <title>Air Quality Webserver</title>
        <meta name='viewport' content='width=device-width, initial-scale=1.0'>
        <link rel='stylesheet' href='https://cdnjs.cloudflare.com/ajax/libs/font-awesome/5.7.2/css/all.min.css'>
        <style>
            body { background-color: #fff; font-family: sans-serif; color: #333333; font: 12px Helvetica, sans-serif box-sizing: border-box;}
            #page { margin: 18px; background-color: #fff;}
            .header { padding: 18px;}
            .header h1 { padding-bottom: 0.3em; color: #00ff00; font-size: 25px; font-weight: bold; text-align: center;}
            h2 { padding-bottom: 0.2em; border-bottom: 1px solid #eee; margin: 2px; text-align: center;}
            
            /* Flexbox to put room boxes side-by-side */
            #content { display: flex; flex-wrap: wrap; justify-content: center; gap: 20px;}
            
            .box-full { padding: 18px; border-radius: 1em; box-shadow: 1px 7px 7px 1px rgba(0,0,0,0.4); background: #fff; width: 300px;}
            .sensor { margin: 10px 0px; font-size: 2.5rem;}
            .sensor-labels { font-size: 1rem; vertical-align: middle; padding-bottom: 15px;}
            .units { font-size: 1.2rem;}
            hr { height: 1px; color: #eee; background-color: #eee; border: none;}
        </style>
        <script>
            // Same thresholds as wifiQualityLabel() in the firmware.
            function wifiLabel(rssi) {
                if (rssi >= -55) return 'Excellent';
                if (rssi >= -65) return 'Good';
                if (rssi >= -75) return 'Fair';
                if (rssi >= -85) return 'Weak';
                return 'Poor';
            }

            // Build one room box. Rooms are discovered from /data, so a new
            // node shows up here automatically — no server edit needed.
            function roomBoxHTML(roomID) {
                const title = roomID.replace(/_/g, ' ');
                return \`
                <h2>\${title} IAQ: <span id='IAQsts_\${roomID}'>Unknown</span></h2>
                <div class='sensors-container'>
                    <div class='sensors'><p class='sensor'><i class='fas fa-thermometer-half' style='color:#0275d8'></i><span class='sensor-labels'> Temperature </span><span id='temperature_\${roomID}'>0</span><span class='units'>°C</span></p><hr></div>
                    <div class='sensors'><p class='sensor'><i class='fas fa-tint' style='color:#0275d8'></i><span class='sensor-labels'> Humidity </span><span id='humidity_\${roomID}'>0</span><span class='units'>%</span></p><hr></div>
                    <div class='sensors'><p class='sensor'><i class='fas fa-tachometer-alt' style='color:#ff0040'></i><span class='sensor-labels'> Pressure </span><span id='pressure_\${roomID}'>0</span><span class='units'>hPa</span></p><hr></div>
                    <div class='sensors'><p class='sensor'><i class='fab fa-cloudversify' style='color:#483d8b'></i><span class='sensor-labels'> IAQ </span><span id='IAQ_\${roomID}'>0</span><span class='units'>PPM</span></p><hr></div>
                    <div class='sensors'><p class='sensor'><i class='fas fa-smog' style='color:#35b22d'></i><span class='sensor-labels'> Co2 Eq. </span><span id='carbon_\${roomID}'>0</span><span class='units'>PPM</span></p><hr></div>
                    <div class='sensors'><p class='sensor'><i class='fas fa-wind' style='color:#0275d8'></i><span class='sensor-labels'> Breath VOC </span><span id='VOC_\${roomID}'>0</span><span class='units'>PPM</span></p><hr></div>
                    <div class='sensors'><p class='sensor'><i class='fas fa-sun' style='color:#f0ad4e'></i><span class='sensor-labels'> Light </span><span id='lux_\${roomID}'>0</span><span class='units'>lx</span></p><hr></div>
                    <div class='sensors'><p class='sensor'><i class='fas fa-volume-up' style='color:#5bc0de'></i><span class='sensor-labels'> Sound </span><span id='soundDb_\${roomID}'>0</span><span class='units'>dB</span></p><p style='font-size: 0.85rem; color: #888; margin: -8px 0 8px 28px;'>avg: <span id='sound_\${roomID}'>0</span> &middot; peak: <span id='soundPeak_\${roomID}'>0</span></p><hr></div>
                    <div class='sensors'><p class='sensor'><i class='fas fa-battery-half' style='color:#5cb85c'></i><span class='sensor-labels'> Battery </span><span id='battery_\${roomID}'>0</span><span class='units'>%</span></p><hr></div>
                    <div class='sensors'><p class='sensor'><i class='fas fa-wifi' style='color:#6f42c1'></i><span class='sensor-labels'> WiFi </span><span id='rssi_\${roomID}'>--</span><span class='units'>dBm</span></p><p style='font-size: 0.85rem; color: #888; margin: -8px 0 8px 28px;'><span id='wifiQuality_\${roomID}'>waiting for data</span></p></div>
                </div>\`;
            }

            function fetchData() {
                fetch('/data')
                .then(response => response.json())
                .then(rooms => {
                    const roomIDs = Object.keys(rooms);
                    const placeholder = document.getElementById('no-rooms');
                    if (placeholder) placeholder.style.display = roomIDs.length ? 'none' : 'block';

                    // Loop through every room sent by the server
                    for (const roomID of roomIDs) {
                        const data = rooms[roomID];
                        // First time we see this room: create its box
                        if (!document.getElementById('box_' + roomID)) {
                            const box = document.createElement('div');
                            box.className = 'box-full';
                            box.id = 'box_' + roomID;
                            box.innerHTML = roomBoxHTML(roomID);
                            document.getElementById('content').appendChild(box);
                        }
                        document.getElementById('temperature_' + roomID).innerText = data.temperature;
                        document.getElementById('humidity_' + roomID).innerText = data.humidity;
                        document.getElementById('pressure_' + roomID).innerText = data.pressure;
                        document.getElementById('IAQ_' + roomID).innerText = data.IAQ;
                        document.getElementById('carbon_' + roomID).innerText = data.carbon;
                        document.getElementById('VOC_' + roomID).innerText = data.VOC;
                        document.getElementById('IAQsts_' + roomID).innerText = data.IAQsts;
                        document.getElementById('lux_' + roomID).innerText = data.lux ?? 0;
                        document.getElementById('sound_' + roomID).innerText = data.sound ?? 0;
                        document.getElementById('soundPeak_' + roomID).innerText = data.soundPeak ?? 0;
                        document.getElementById('soundDb_' + roomID).innerText = (data.soundDb ?? 0).toFixed ? (data.soundDb ?? 0).toFixed(1) : (data.soundDb ?? 0);
                        document.getElementById('battery_' + roomID).innerText = data.battery ?? 0;
                        // rssi is null when the node runs firmware that predates
                        // the WiFi-quality field — flag it instead of showing 0.
                        if (data.rssi !== null && data.rssi !== undefined) {
                            document.getElementById('rssi_' + roomID).innerText = data.rssi;
                            document.getElementById('wifiQuality_' + roomID).innerText =
                                (data.wifiPercent ?? '?') + '% · ' + wifiLabel(data.rssi);
                        } else {
                            document.getElementById('rssi_' + roomID).innerText = '--';
                            document.getElementById('wifiQuality_' + roomID).innerText =
                                'not reported (update firmware)';
                        }
                    }
                })
                .catch(error => console.error('Error fetching data:', error));
            }
            setInterval(fetchData, 1000);
            document.addEventListener('DOMContentLoaded', fetchData);
        </script>
    </head>
    <body>
        <div id='page'>
            <div class='header'>
                <h1>Air Quality Monitoring System</h1>
            </div>
            <div id='content'>
                <p id='no-rooms' style='color:#888'>Waiting for the first sensor node to report...</p>
            </div>
        </div>
    </body>
    </html>
  `;
}

app.listen(3000, () => console.log('Server running on http://localhost:3000'));