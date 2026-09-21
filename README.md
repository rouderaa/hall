<html>
<head>
<meta http-equiv="content-type" content="text/html; charset=utf-8">
<meta name="generator" content="ReText 8.0.2">
</head>
<body>
<h1>MT6835 Angle Sensor on ESP32-S3</h1>
<p>An Arduino sketch that reads the 21-bit absolute angle from an <strong>MT6835</strong> magnetic angle sensor over SPI on an <strong>ESP32-S3</strong>. It verifies the chip ID at startup, decodes the angle in degrees, checks the CRC, reports the sensor's status flags, and prints one reading per second to the serial port.</p>
<p><img src="assets/hall_overview.JPG" alt="hall sensor testrig" style="width: 400px; max-width: 100%;"></p>
<p>The axle of the testrig consists of several parts that have been press fit using a vise.</p>
<p><img src="assets/axle_parts.JPG" alt="axle parts" style="width: 400px; max-width: 100%;"></p>
<p>A movie of the testrig is available <a href="https://www.youtube.com/watch?v=AIi2X_5kkNY">here</a> .
The freecad 1.1 design is available in the design directory.</p>
<h2>Features</h2>
<ul>
<li>Reads the 21-bit angle (about 0.00017° per step) and converts it to degrees</li>
<li>Validates every sample with the sensor's CRC-8 (poly <code>0x07</code>)</li>
<li>Decodes the status bits: over-speed, weak magnetic field, undervoltage</li>
<li>Hardware SPI on <code>HSPI</code> (SPI3) with manual chip-select</li>
<li>Automatic fallback to bit-banged SPI if the hardware path doesn't return the expected chip ID</li>
<li>Step-by-step startup log that makes wiring problems easy to spot</li>
<li>CRLF line endings so output displays correctly in raw terminals such as <code>picocom</code></li>
</ul>
<h2>Hardware</h2>
<ul>
<li>ESP32-S3 board</li>
<li>MT6835 magnetic angle sensor (breakout or custom board) with a diametrically magnetized magnet over the chip</li>
</ul>
<h3>Wiring</h3>
<table>
<thead>
<tr>
<th>Signal</th>
<th>ESP32-S3 GPIO</th>
<th>MT6835 pin</th>
</tr>
</thead>
<tbody>
<tr>
<td>SCK</td>
<td>1</td>
<td>SCK</td>
</tr>
<tr>
<td>MISO</td>
<td>12</td>
<td>SDO</td>
</tr>
<tr>
<td>MOSI</td>
<td>11</td>
<td>SDI</td>
</tr>
<tr>
<td>CS</td>
<td>6</td>
<td>CSN</td>
</tr>
<tr>
<td>CAL</td>
<td>7</td>
<td>CAL</td>
</tr>
<tr>
<td>3.3 V / GND</td>
<td>-</td>
<td>VDD / GND</td>
</tr>
</tbody>
</table>
<p>Pins are set with the <code>PIN_*</code> defines at the top of the sketch. Power the sensor so its logic levels are compatible with the ESP32-S3's 3.3 V I/O.</p>
<p><strong>CAL</strong> is driven <code>LOW</code> by the sketch for normal run mode. <code>HIGH</code> puts the sensor into calibration mode, which this sketch never uses.</p>
<h2>Building and flashing</h2>
<ol>
<li>Install the ESP32 board package (Arduino-ESP32) in the Arduino IDE, or use PlatformIO with an ESP32-S3 environment.</li>
<li>Select your ESP32-S3 board.</li>
<li>Open the sketch, then build and upload it. No third-party libraries are needed; it only uses the built-in <code>SPI</code> library.</li>
</ol>
<h2>Viewing the output</h2>
<p>Open a serial terminal at <strong>115200 baud</strong>. For example, with <code>picocom</code> (the port name will vary on your system):</p>
<pre><code>picocom -b 115200 /dev/ttyACM0
</code></pre>
<p>Sample output (illustrative):</p>
<pre><code>=== MT6835 angle sensor on ESP32-S3 ===
[1/6] Serial @ 115200 baud ready
[2/6] pins: CAL=7 MISO=12 MOSI=11 SCK=1 CSN=6
      CAL -&gt; LOW (run mode)
[3/6] SPI begin (HSPI/SPI3, MODE0, manual CS)
[4/6] reading ID register (settle + retry)
      +100 ms  ID=0x31 (match)
[5/6] hardware SPI path OK (bit-bang not needed)
[6/6] MT6835 ID = 0x31 (expect 0x31) -&gt; detected
---- ready, sampling every 1 s ----
123.456 deg  [ok]

123.457 deg  [ok]
</code></pre>
<p>Each reading is printed as <code>&lt;angle&gt; deg  [&lt;quality&gt;]</code>. The quality field is one of:</p>
<table>
<thead>
<tr>
<th>Quality</th>
<th>Meaning</th>
</tr>
</thead>
<tbody>
<tr>
<td><code>ok</code></td>
<td>CRC matches and no status flags are set</td>
</tr>
<tr>
<td><code>crc-fail</code></td>
<td>The CRC computed over the angle bytes doesn't match</td>
</tr>
<tr>
<td><code>over-speed</code></td>
<td>Status bit 0: shaft is rotating too fast for the sensor</td>
</tr>
<tr>
<td><code>weak-field</code></td>
<td>Status bit 1: magnetic field too weak (check magnet gap)</td>
</tr>
<tr>
<td><code>undervoltage</code></td>
<td>Status bit 2: supply voltage too low</td>
</tr>
</tbody>
</table>
<p>Multiple flags can appear together, separated by spaces.</p>
<h2>How it works</h2>
<h3>Startup sequence</h3>
<ol>
<li>Start serial at 115200 baud.</li>
<li>Print the pin map and drive CAL low.</li>
<li>Initialise <code>HSPI</code> with <code>SCK/MISO/MOSI</code> and no hardware CS (<code>ss = -1</code>); CS is toggled manually.</li>
<li>Read the ID register (<code>0x01</code>) up to 6 times, waiting 100 ms, 200 ms, ... 600 ms before each attempt, and stop as soon as it returns <code>0x31</code>.</li>
<li>If the hardware path never returns <code>0x31</code>, switch to bit-banged SPI and try once more.</li>
<li>Print the final detection result and start sampling.</li>
</ol>
<h3>SPI transaction</h3>
<p>Register reads use SPI mode 0 at 2 MHz with a 3-byte frame: <code>0x30, &lt;register&gt;, 0x00</code>. The register value is the third byte clocked back from the sensor. Chip select is asserted manually, with short delays around it.</p>
<p>The sketch deliberately does not let the SPI peripheral own CS: on the wiring this was written for, doing so left MISO floating.</p>
<h3>Registers used</h3>
<table>
<thead>
<tr>
<th>Address</th>
<th>Name</th>
<th>Description</th>
</tr>
</thead>
<tbody>
<tr>
<td><code>0x01</code></td>
<td>ID</td>
<td>Chip ID, expected <code>0x31</code></td>
</tr>
<tr>
<td><code>0x03</code></td>
<td>A3</td>
<td>Angle bits 20..13</td>
</tr>
<tr>
<td><code>0x04</code></td>
<td>A2</td>
<td>Angle bits 12..5</td>
</tr>
<tr>
<td><code>0x05</code></td>
<td>A1</td>
<td>Angle bits 4..0 (upper bits) and status (low 3)</td>
</tr>
<tr>
<td><code>0x06</code></td>
<td>CRC</td>
<td>CRC-8 over A3, A2, A1</td>
</tr>
</tbody>
</table>
<h3>Angle decoding</h3>
<pre><code>raw     = (A3 &lt;&lt; 13) | (A2 &lt;&lt; 5) | (A1 &gt;&gt; 3)      // 21-bit value
degrees = raw / 2^21 * 360
status  = A1 &amp; 0x07                               // bit0 over-speed, bit1 weak-field, bit2 undervoltage
</code></pre>
<p>The CRC is CRC-8 with polynomial <code>0x07</code>, initial value <code>0x00</code>, MSB-first, no reflection and no final XOR, computed over <code>A3, A2, A1</code>.</p>
<h2>Configuration</h2>
<table>
<thead>
<tr>
<th>Define</th>
<th>Default</th>
<th>Purpose</th>
</tr>
</thead>
<tbody>
<tr>
<td><code>PIN_CAL</code></td>
<td><code>7</code></td>
<td>Calibration-mode select (kept low)</td>
</tr>
<tr>
<td><code>PIN_MISO</code></td>
<td><code>12</code></td>
<td>SPI data from sensor</td>
</tr>
<tr>
<td><code>PIN_MOSI</code></td>
<td><code>11</code></td>
<td>SPI data to sensor</td>
</tr>
<tr>
<td><code>PIN_SCK</code></td>
<td><code>1</code></td>
<td>SPI clock</td>
</tr>
<tr>
<td><code>PIN_CS</code></td>
<td><code>6</code></td>
<td>Chip select (manual)</td>
</tr>
<tr>
<td><code>MT_ID_DEFAULT</code></td>
<td><code>0x31</code></td>
<td>Expected chip ID</td>
</tr>
<tr>
<td><code>SPI_FREQ</code></td>
<td><code>2000000</code></td>
<td>Hardware SPI clock in Hz (500 kHz to 2 MHz tested)</td>
</tr>
</tbody>
</table>
<p>The sampling interval is fixed at 1 s in <code>loop()</code>.</p>
<h2>Troubleshooting</h2>
<ul>
<li><strong><code>ID=0xFF</code> or <code>0x00</code> on every attempt</strong>: check wiring, especially MISO and CS, and confirm the sensor is powered. If the hardware path fails, the sketch retries with bit-banged SPI automatically; look at the <code>[5/6]</code> line to see which path is in use.</li>
<li><strong><code>weak-field</code></strong>: adjust the distance or alignment between the magnet and the chip.</li>
<li><strong>Occasional <code>crc-fail</code> while the shaft is moving</strong>: the four registers are read in separate SPI transactions, so a fast-moving shaft can produce a sample whose bytes come from slightly different positions. Such samples are flagged rather than silently accepted. A single burst read of the angle registers, if your datasheet revision supports it, would avoid this.</li>
<li><strong>Garbled or staircased terminal output</strong>: make sure your terminal is set to 115200 baud. Lines are terminated with <code>\r\n</code> for raw terminals.</li>
</ul>
<h2>Limitations</h2>
<ul>
<li>Read-only: the sketch does not write to sensor registers or EEPROM, and does not perform calibration.</li>
</ul>

</body>
</html>
