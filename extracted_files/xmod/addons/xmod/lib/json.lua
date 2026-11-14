-- xmod json library:  local json = require("json");  json.encode(value [, pretty])  /  json.decode(string) -> value | nil, err
local J = {}
local esc = { ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
local function enc_str(s) return '"' .. s:gsub('[%c"\\]', function(c) return esc[c] or string.format("\\u%04x", c:byte()) end) .. '"' end
local function is_array(t) local n = 0; for k in pairs(t) do if type(k) ~= "number" or k < 1 or k % 1 ~= 0 then return false end; n = n + 1 end; return n == #t end
local function enc(v, pretty, ind)
  local t = type(v)
  if t == "nil" then return "null"
  elseif t == "boolean" then return tostring(v)
  elseif t == "number" then if v ~= v or v == math.huge or v == -math.huge then return "null" end; return (math.type(v) == "integer") and tostring(v) or string.format("%.14g", v)
  elseif t == "string" then return enc_str(v)
  elseif t == "table" then
    local nl, pad, pad2 = "", "", ""
    if pretty then nl = "\n"; pad = string.rep("  ", ind + 1); pad2 = string.rep("  ", ind) end
    local out = {}
    if is_array(v) then
      for i = 1, #v do out[#out + 1] = pad .. enc(v[i], pretty, ind + 1) end
      if #out == 0 then return "[]" end
      return "[" .. nl .. table.concat(out, "," .. nl) .. nl .. pad2 .. "]"
    end
    local keys = {}
    for k in pairs(v) do keys[#keys + 1] = tostring(k) end
    table.sort(keys)
    for _, k in ipairs(keys) do local val = v[k]; if val == nil then val = v[tonumber(k)] end; out[#out + 1] = pad .. enc_str(k) .. (pretty and ": " or ":") .. enc(val, pretty, ind + 1) end
    if #out == 0 then return "{}" end
    return "{" .. nl .. table.concat(out, "," .. nl) .. nl .. pad2 .. "}"
  end
  error("cannot encode " .. t)
end
function J.encode(v, pretty) return enc(v, pretty, 0) end

local function skip(s, i) return s:find("[^ \t\r\n]", i) or #s + 1 end
local dec
local function dec_str(s, i)
  local out, j = {}, i + 1
  while true do
    local c = s:sub(j, j)
    if c == "" then error("unterminated string") end
    if c == '"' then return table.concat(out), j + 1 end
    if c == "\\" then
      local n = s:sub(j + 1, j + 1)
      if n == "u" then out[#out + 1] = utf8.char(tonumber(s:sub(j + 2, j + 5), 16)); j = j + 6
      else out[#out + 1] = ({ b = "\b", f = "\f", n = "\n", r = "\r", t = "\t" })[n] or n; j = j + 2 end
    else out[#out + 1] = c; j = j + 1 end
  end
end
function dec(s, i)
  i = skip(s, i); local c = s:sub(i, i)
  if c == "{" then
    local t = {}; i = skip(s, i + 1)
    if s:sub(i, i) == "}" then return t, i + 1 end
    while true do
      i = skip(s, i); if s:sub(i, i) ~= '"' then error("object key expected at " .. i) end
      local k; k, i = dec_str(s, i); i = skip(s, i)
      if s:sub(i, i) ~= ":" then error("':' expected at " .. i) end
      t[k], i = dec(s, i + 1); i = skip(s, i)
      local d = s:sub(i, i); i = i + 1
      if d == "}" then return t, i elseif d ~= "," then error("',' or '}' expected at " .. (i - 1)) end
    end
  elseif c == "[" then
    local t = {}; i = skip(s, i + 1)
    if s:sub(i, i) == "]" then return t, i + 1 end
    while true do
      t[#t + 1], i = dec(s, i); i = skip(s, i)
      local d = s:sub(i, i); i = i + 1
      if d == "]" then return t, i elseif d ~= "," then error("',' or ']' expected at " .. (i - 1)) end
    end
  elseif c == '"' then return dec_str(s, i)
  elseif s:sub(i, i + 3) == "true" then return true, i + 4
  elseif s:sub(i, i + 4) == "false" then return false, i + 5
  elseif s:sub(i, i + 3) == "null" then return nil, i + 4
  else
    local num = s:match("^-?%d+%.?%d*[eE]?[+-]?%d*", i)
    if not num or num == "" then error("unexpected character at " .. i) end
    return tonumber(num), i + #num
  end
end
function J.decode(s)
  local ok, v = pcall(function() local val, i = dec(s, 1); i = skip(s, i); if i <= #s then error("trailing garbage at " .. i) end; return val end)
  if ok then return v end
  return nil, v
end
return J
