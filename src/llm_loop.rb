#!/usr/bin/env ruby
# frozen_string_literal: true

require 'json'
require 'net/http'
require 'uri'
require 'time'
require 'timeout'
require 'thread'
require 'open3'

$stdout.sync = true

begin
  require 'serialport'
rescue LoadError
  SerialPort = nil
end

puts "Loaded llm_loop.py from: #{__FILE__}"

OLLAMA_HOST = ENV.fetch('OLLAMA_HOST', 'http://127.0.0.1:11434')
OLLAMA_BASE = OLLAMA_HOST.end_with?('/') ? OLLAMA_HOST : "#{OLLAMA_HOST}/"
MODEL       = ENV.fetch('OLLAMA_MODEL', 'mistral:7b-instruct')

TRANSPORT   = ENV.fetch('TRANSPORT', 'BLE').strip.upcase
ESP32_URL   = ENV['ESP32_URL']
SERIAL_PORT = ENV['SERIAL_PORT']
BAUD        = ENV.fetch('SERIAL_BAUD', '115200').to_i

BLE_NAME    = ENV.fetch('BLE_NAME', 'MatrixPanel')
BLE_ADDRESS = ENV['BLE_ADDRESS']
BLE_ENABLED = (TRANSPORT == 'BLE') || (!BLE_NAME.to_s.empty? || !BLE_ADDRESS.to_s.empty?)
NUS_SERVICE_UUID = '6E400001-B5A3-F393-E0A9-E50E24DCCA9E'
NUS_RX_CHAR_UUID = '6E400002-B5A3-F393-E0A9-E50E24DCCA9E'

INTERVAL_S = ENV.fetch('INTERVAL_S', '60').to_i

PANEL_COLS = ENV.fetch('PANEL_COLS', '21').to_i
MAX_TOKENS = ENV.fetch('MAX_TOKENS', '28').to_i
MAX_LINES  = ENV.fetch('MAX_LINES', '6').to_i

PROMPT = <<~PROMPT_TEXT.freeze
  You must obey this FORMAT CONTRACT exactly.

  OBJECTIVE
Write one coherent sentence across AT MOST 28 tokens delivering a line that sounds like it was lifted 
from the book THE OUTSIDERS by S.E. Hinton
End naturally when the sentence is complete; do not pad to reach 28 tokens.

  HARD RULES
  - Output no more than 6 (SIX) lines.
  - After the final line, print a separate control line: END
  - The END control line does not count toward the 6 line limit.
  - Each line MUST be less than 20 characters
  - Do NOT split or hyphenate words.
  - Apostrophes within words, commas, and full stops are allowed; no other punctuation, digits, emojis, or symbols.
  - The FIRST WORD on line 1 must be a simple concrete noun and MUST NOT be the same as your previous answer.
  - The six lines together must read as a single sentence.
  - Only one sentence total. End the sentence with a full stop. Do not continue beyond the first full stop.
  - Check that there are no more than 6 lines.
  - Check that there are no more than 28 tokens across the lines.

  SELF-CHECK BEFORE YOU PRINT
  If any rule is violated, stop, re-generate, then print. Always end on a complete sentence.
PROMPT_TEXT

TOKEN_RE = /\G[A-Za-z]+(?:'[A-Za-z]+)*/

$ser_handle = nil
$ble_mutex = Mutex.new
$ble_persist = nil
BLE_DEBUG = ENV.fetch('BLE_DEBUG', nil) == '1'

PYTHON_BLE_BRIDGE = <<~'PYCODE'
import asyncio
import json
import sys

NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
NUS_RX_CHAR_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

try:
    from bleak import BleakClient, BleakScanner
except Exception as exc:
    print(json.dumps({"status": "error", "error": f"bleak not installed: {exc}"}), flush=True)
    sys.exit(1)

state = {"client": None, "target": None, "name": None, "address": None, "rx_char": None}

async def discover_target():
    if state["target"]:
        return state["target"]
    name = state["name"]
    address = state["address"]
    if address:
        state["target"] = address
        return address
    try:
        if hasattr(BleakScanner, "find_device_by_filter"):
            def _flt(device, adv_data):
                if name and (device.name == name or (device.name or "").startswith(name)):
                    return True
                uuids = []
                try:
                    uuids = (adv_data and adv_data.service_uuids) or []
                except Exception:
                    uuids = []
                for u in uuids:
                    if str(u).lower() == NUS_SERVICE_UUID.lower():
                        return True
                return False
            dev = await BleakScanner.find_device_by_filter(_flt, timeout=8.0)
            if dev is not None:
                state["target"] = dev.address
                return state["target"]
    except Exception:
        pass
    devices = await BleakScanner.discover(timeout=10.0)
    names = []
    for dev in devices:
        if name and (dev.name == name or (dev.name or "").startswith(name)):
            state["target"] = dev.address
            return state["target"]
        names.append((dev.address, dev.name or ""))
    for dev in devices:
        uuids = []
        try:
            uuids = dev.metadata.get("uuids") or []
        except Exception:
            uuids = []
        for uuid in uuids:
            if (uuid or "").lower() == NUS_SERVICE_UUID.lower():
                state["target"] = dev.address
                return state["target"]
        names.append((dev.address, dev.name or ""))
    print(json.dumps({"status": "debug", "event": "scan", "devices": names}), flush=True)
    raise RuntimeError(f"BLE target not found (name={name!r} address={address!r})")

async def ensure_connected():
    client = state["client"]
    if client is not None and client.is_connected:
        if state.get("rx_char") is None:
            try:
                services = await client.get_services()
                rx = services.get_characteristic(NUS_RX_CHAR_UUID)
                if rx:
                    state["rx_char"] = rx
            except Exception:
                pass
        return client
    target = await discover_target()
    if client is not None:
        try:
            await client.disconnect()
        except Exception:
            pass
    cli = BleakClient(target, timeout=10.0)
    try:
        await cli.connect()
        services = await cli.get_services()
    except Exception:
        state["target"] = None
        target = await discover_target()
        cli = BleakClient(target, timeout=12.0)
        await cli.connect()
        services = await cli.get_services()
    if not cli.is_connected:
        raise RuntimeError("BLE connect failed")
    try:
        rx = services.get_characteristic(NUS_RX_CHAR_UUID)
    except Exception as exc:
        print(f"[ble-helper] failed to get characteristic: {exc}", file=sys.stderr, flush=True)
        rx = None
    if rx is None:
        try:
            svc_list = [svc.uuid for svc in services]
        except Exception:
            svc_list = []
        print(f"[ble-helper] services discovered: {svc_list}", file=sys.stderr, flush=True)
        raise RuntimeError(f"Characteristic {NUS_RX_CHAR_UUID} not found after service discovery")
    state["client"] = cli
    state["rx_char"] = rx
    return cli

async def write_payload(payload):
    client = await ensure_connected()
    data = payload.encode("ascii", "ignore")
    char = state.get("rx_char")
    if char is None:
        services = await client.get_services()
        char = services.get_characteristic(NUS_RX_CHAR_UUID)
        if char is None:
            raise RuntimeError(f"Characteristic {NUS_RX_CHAR_UUID} not available on connected client")
        state["rx_char"] = char
    try:
        await client.write_gatt_char(char, data, response=True)
    except Exception:
        state["client"] = None
        state["rx_char"] = None
        client = await ensure_connected()
        char = state.get("rx_char")
        if char is None:
            services = await client.get_services()
            char = services.get_characteristic(NUS_RX_CHAR_UUID)
            if char is None:
                raise RuntimeError(f"Characteristic {NUS_RX_CHAR_UUID} not available after reconnect")
            state["rx_char"] = char
        await client.write_gatt_char(char, data, response=True)

async def close_client():
    client = state.get("client")
    if client is not None:
        try:
            await client.disconnect()
        except Exception:
            pass
        state["client"] = None
    state["rx_char"] = None

async def handle_command(cmd):
    cmd_type = cmd.get("type")
    if cmd_type == "config":
        state["name"] = cmd.get("name")
        state["address"] = cmd.get("address")
        if state["address"]:
            state["target"] = state["address"]
        return "configured"
    if cmd_type == "connect":
        await ensure_connected()
        return "connected"
    if cmd_type == "write":
        payload = cmd.get("payload", "")
        await write_payload(payload)
        return "written"
    if cmd_type == "close":
        await close_client()
        return "closed"
    raise RuntimeError(f"Unknown command type: {cmd_type}")

loop = asyncio.new_event_loop()
asyncio.set_event_loop(loop)

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        cmd = json.loads(line)
    except Exception as exc:
        print(json.dumps({"status": "error", "error": f"invalid json: {exc}"}), flush=True)
        continue
    try:
        result = loop.run_until_complete(handle_command(cmd))
        print(json.dumps({"status": "ok", "result": result}), flush=True)
        if cmd.get("type") == "close":
            break
    except Exception as exc:
        print(json.dumps({"status": "error", "error": str(exc)}), flush=True)
        if cmd.get("type") == "close":
            break

loop.run_until_complete(close_client())
PYCODE

class BlePersistent
  def initialize_helper(name, address)
    @name = name
    @address = address
    @connected = false
    @stdin, @stdout, @wait_thr = Open3.popen2('python3', '-u', '-c', PYTHON_BLE_BRIDGE)
    if (line = try_read_line(0.1))
      begin
        resp = JSON.parse(line)
        if resp['status'] == 'error'
          msg = resp['error'] || 'BLE helper error'
          raise 'bleak not installed. Install with: pip install bleak' if msg.include?('bleak not installed')
          raise msg
        end
      rescue JSON::ParserError
        # Ignore non-JSON output
      end
    end
    send_command(type: 'config', name: name, address: address)
    read_ok
  rescue Errno::ENOENT
    raise 'python3 not found; required for BLE transport helper'
  end
  private :initialize_helper

  def initialize(name, address)
    initialize_helper(name, address)
  rescue StandardError => e
    cleanup
    raise e
  end

  def connect
    return if @connected
    send_command(type: 'connect')
    read_ok
    @connected = true
  end

  def write(payload)
    connect unless @connected
    send_command(type: 'write', payload: payload)
    read_ok
  rescue StandardError => e
    @connected = false
    raise e
  end

  def close
    begin
      send_command(type: 'close')
      read_ok
    rescue StandardError
      # Ignore errors during shutdown
    ensure
      @connected = false
      cleanup
    end
  end

  private

  def cleanup
    begin
      @stdin&.close unless @stdin&.closed?
    rescue StandardError
      nil
    end
    begin
      @stdout&.close unless @stdout&.closed?
    rescue StandardError
      nil
    end
    begin
      @wait_thr&.value
    rescue StandardError
      nil
    end
  end

  def ensure_alive!
    raise 'BLE helper process has exited' unless @wait_thr&.alive?
  end

  def send_command(hash)
    ensure_alive!
    @stdin.puts(JSON.generate(hash))
    @stdin.flush
  end

  def read_ok
    resp = read_response
    if resp['status'] == 'ok'
      resp['result']
    else
      raise(resp['error'] || 'BLE helper returned error')
    end
  end

  def read_response
    line = @stdout.gets
    raise 'BLE helper terminated unexpectedly' unless line
    JSON.parse(line)
  rescue JSON::ParserError => e
    raise "Invalid response from BLE helper: #{e.message}"
  end

  def try_read_line(timeout)
    readers, = IO.select([@stdout], nil, nil, timeout)
    return nil unless readers
    @stdout.gets
  end
end

def build_prompt
  letters = 'ABCDEFGHIJKLMNOPQRSTUVWY'
  first = letters[Time.now.utc.min % 24]
  "#{PROMPT}\nEXTRA RULE: The very first word must start with '#{first}'."
end

def http_request(method, path, body: nil, headers: {}, open_timeout: 5, read_timeout: 5)
  uri = URI.join(OLLAMA_BASE, path)
  http = Net::HTTP.new(uri.host, uri.port)
  http.use_ssl = (uri.scheme == 'https')
  http.open_timeout = open_timeout
  http.read_timeout = read_timeout
  request =
    case method
    when :get
      Net::HTTP::Get.new(uri)
    when :post
      Net::HTTP::Post.new(uri)
    else
      raise ArgumentError, "Unsupported HTTP method: #{method}"
    end
  headers.each { |k, v| request[k] = v }
  request.body = body if body
  http.request(request)
end

def ensure_ollama_up
  response = http_request(:get, '/api/tags', open_timeout: 3, read_timeout: 3)
  response.value
  true
rescue StandardError => e
  puts "Ollama not reachable at #{OLLAMA_HOST} - #{e}"
  false
end

def has_model(model_name)
  response = http_request(:get, '/api/tags', open_timeout: 5, read_timeout: 5)
  response.value
  body = JSON.parse(response.body)
  models = body['models']
  return false unless models.is_a?(Array)

  models.any? { |m| m.is_a?(Hash) && m['name'] == model_name }
rescue StandardError
  false
end

def wrap_to_width(text, cols)
  words = text.split(/\s+/)
  return '' if words.empty?

  lines = []
  current = ''
  words.each do |word|
    if word.length > cols
      lines << current unless current.empty?
      lines << word
      current = ''
      next
    end

    if current.empty?
      current = word
    elsif current.length + 1 + word.length <= cols
      current << ' ' << word
    else
      lines << current
      current = word
    end
  end
  lines << current unless current.empty?
  lines.join("\n")
end

def sanitize_and_format(raw, max_tokens, max_lines, width)
  lines = raw.split("\n")
  lines.pop while lines.any? && lines.last.strip.empty?
  lines.pop if lines.any? && lines.last.strip == 'END'

  text = lines.map { |ln| ln.strip }.reject(&:empty?).join(' ')
  text = text.gsub("\u2019", "'").gsub("\u2018", "'")

  if (dot_idx = text.index('.'))
    text = text[0..dot_idx]
  end

  out_chars = []
  word_count = 0
  i = 0
  n = text.length
  last_was_space = false

  while i < n
    match = TOKEN_RE.match(text, i)
    if match
      break if word_count >= max_tokens
      token = match[0]
      out_chars << ' ' if !out_chars.empty? && ![' ', ',', '.'].include?(out_chars.last)
      out_chars << token
      word_count += 1
      i = match.end(0)
      last_was_space = false
      next
    end

    ch = text[i]
    if ch =~ /\s/
      if !out_chars.empty? && !last_was_space
        out_chars << ' '
        last_was_space = true
      end
    elsif ch == ',' || ch == '.'
      unless out_chars.empty?
        out_chars.pop if out_chars.last == ' '
        out_chars << ch
        out_chars << ' '
        last_was_space = true
      end
    end
    i += 1
  end

  trimmed = out_chars.join.strip
  if !trimmed.empty? && trimmed[-1] != '.'
    trimmed = trimmed.sub(/[\s,]+$/, '')
    trimmed << '.'
  end

  wrapped = wrap_to_width(trimmed, width)
  out_lines = wrapped.split("\n")
  out_lines = out_lines.first(max_lines)
  out_lines.join("\n")
end

def ollama_generate
  begin
    if ensure_ollama_up && has_model(MODEL)
      payload = {
        model: MODEL,
        messages: [{ role: 'user', content: build_prompt }],
        stream: false,
        options: {
          temperature: 0.45,
          top_p: 0.9,
          top_k: 40,
          repeat_penalty: 1.5,
          stop: ["\nEND"],
          num_predict: 64
        }
      }
      response = http_request(
        :post,
        '/api/chat',
        body: JSON.generate(payload),
        headers: { 'Content-Type' => 'application/json' },
        open_timeout: 10,
        read_timeout: 120
      )
      if response.code.to_i != 404
        response.value
        json = JSON.parse(response.body)
        if json.is_a?(Hash)
          if json['message'].is_a?(Hash) && json['message']['content']
            return json['message']['content']
          elsif json['response']
            return json['response']
          end
        end
      end
    end
  rescue StandardError => e
    puts "HTTP API path failed (falling back to CLI): #{e}"
  end

  begin
    stdout = ''
    stderr = ''
    status = nil
    Timeout.timeout(180) do
      stdout, stderr, status = Open3.capture3('ollama', 'run', MODEL, PROMPT)
    end
    return stdout if status&.success?

    err = stderr.to_s.strip
    err = 'unknown error' if err.empty?
    raise "ollama run failed: #{err}"
  rescue Errno::ENOENT
    raise 'Ollama CLI not found. Install with: brew install ollama'
  end
end

def send_http(payload)
  raise 'ESP32_URL not set' if ESP32_URL.nil? || ESP32_URL.empty?

  uri = URI.parse(ESP32_URL)
  request = Net::HTTP::Post.new(uri)
  request['Content-Type'] = 'text/plain'
  request.body = payload.encode('ASCII', invalid: :replace, undef: :replace, replace: '')
  http = Net::HTTP.new(uri.host, uri.port)
  http.use_ssl = (uri.scheme == 'https')
  http.open_timeout = 5
  http.read_timeout = 10
  response = http.request(request)
  response.value
  response.body
end

def send_serial(payload)
  raise 'SERIAL_PORT not set' if SERIAL_PORT.nil? || SERIAL_PORT.empty?
  raise 'serialport gem not installed' unless defined?(SerialPort) && SerialPort

  unless $ser_handle && $ser_handle.respond_to?(:closed?) && !$ser_handle.closed?
    $ser_handle = SerialPort.new(SERIAL_PORT, BAUD)
    begin
      $ser_handle.dtr = 0 if $ser_handle.respond_to?(:dtr=)
      $ser_handle.rts = 0 if $ser_handle.respond_to?(:rts=)
    rescue StandardError
      nil
    end
    sleep 0.2
  end

  ascii_payload = payload.encode('ASCII', invalid: :replace, undef: :replace, replace: '')
  bytes = $ser_handle.write(ascii_payload)
  $ser_handle.flush
  puts "SERIAL wrote #{bytes} bytes"
  'ok-serial'
end

def send_ble(payload)
  $ble_mutex.synchronize do
    $ble_persist ||= BlePersistent.new(BLE_NAME, BLE_ADDRESS)
    $ble_persist.connect
    if payload.empty?
      puts('[ble debug] skip empty payload write') if BLE_DEBUG
    else
      puts("[ble debug] write payload bytes=#{payload.bytesize}") if BLE_DEBUG
      $ble_persist.write(payload)
    end
  end
  'ok-ble'
end

at_exit do
  begin
    $ble_mutex.synchronize do
      $ble_persist&.close
      $ble_persist = nil
    end
  rescue StandardError
    nil
  end
  begin
    $ser_handle&.close if $ser_handle && $ser_handle.respond_to?(:close)
  rescue StandardError
    nil
  end
end

def main
  transport = TRANSPORT
  transport = 'BLE' unless %w[BLE HTTP USB].include?(transport)
  puts "Script: #{__FILE__} | Transport: #{transport} | Model: #{MODEL} | Host: #{OLLAMA_HOST} | API+CLI fallback"

  if transport == 'HTTP' && (ESP32_URL.nil? || ESP32_URL.empty?)
    puts 'Set ESP32_URL for HTTP transport.'
    exit 1
  end
  if transport == 'USB' && (SERIAL_PORT.nil? || SERIAL_PORT.empty?)
    puts 'Set SERIAL_PORT for USB transport.'
    exit 1
  end

  if transport == 'USB' && SERIAL_PORT && defined?(SerialPort) && SerialPort
    begin
      send_serial('')
    rescue StandardError => e
      puts "Warning: initial serial open failed: #{e}"
    end
  elsif transport == 'USB'
    puts 'serialport gem not installed. Install with: gem install serialport'
    exit 1
  end

  if transport == 'BLE'
    begin
      send_ble('')
      puts 'BLE connected (persistent)'
    rescue StandardError => e
      puts "Initial BLE connect failed: #{e}"
    end
  end

  loop do
    begin
      raw = ollama_generate
      width = [PANEL_COLS, 19].min
      formatted = sanitize_and_format(raw, MAX_TOKENS, MAX_LINES, width)
      line_count = formatted.split("\n").count { |ln| !ln.strip.empty? }
      puts "Processed lines: #{line_count}"

      payload = formatted.end_with?("\n") ? formatted : "#{formatted}\n"
      puts "Generated (wrapped to #{width} cols):\n#{payload}"

      case transport
      when 'HTTP'
        resp = send_http(payload)
        puts "HTTP -> #{resp}"
      when 'BLE'
        resp = send_ble(payload)
        puts "BLE -> #{resp}"
      else
        resp = send_serial(payload)
        puts "SERIAL -> #{resp}"
      end
    rescue StandardError => e
      puts "Error (#{e.class}): #{e.message}"
      if BLE_DEBUG && e.backtrace
        puts "[ble debug] backtrace: #{e.backtrace.first}"
      end
    end

    sleep INTERVAL_S
  end
end

main if __FILE__ == $PROGRAM_NAME
