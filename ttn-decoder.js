// TTN Console -> Application -> Payload formatters -> Uplink -> Custom JavaScript.
// Matches the payload built in src/main.cpp (FPort 1, 5 bytes).
function decodeUplink(input) {
  var b = input.bytes;
  if (b.length < 5) {
    return { errors: ["expected 5 bytes, got " + b.length] };
  }
  var reasons = ["periodic", "switch", "button"];
  var data = { counter: (b[3] << 8) | b[4], reason: reasons[b[2]] || b[2] };
  for (var i = 0; i < 4; i++) {
    data["sw" + (i + 1)] = (b[0] >> i) & 1 ? "closed" : "open";
    data["sw" + (i + 1) + "_toggled"] = ((b[1] >> i) & 1) === 1;
  }
  return { data: data };
}
