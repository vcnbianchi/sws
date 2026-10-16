done = function(summary, latency, requests)
  local seconds = summary.duration / 1e6
  local errors = summary.errors
  io.write(string.format(
    "WRK_CSV,%d,%d,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%d\n",
    summary.requests,
    summary.bytes,
    summary.requests / seconds,
    latency.mean / 1000,
    latency.stdev / 1000,
    latency:percentile(50) / 1000,
    latency:percentile(90) / 1000,
    latency:percentile(99) / 1000,
    latency:percentile(99.9) / 1000,
    latency.max / 1000,
    errors.connect,
    errors.read,
    errors.write,
    errors.status,
    errors.timeout))
end
