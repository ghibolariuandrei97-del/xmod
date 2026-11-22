-- xmod menu library:  local menu = require("menu")
-- menu.show(id, { title="Pick", items={ {"Label", value}, ... }, on_select=function(id, label, value) end, timeout=-1 })
local M = {}
local open = {}                       -- id -> menu state
local PER_PAGE = 7

local function render(id)
  local st = open[id]; if not st then return end
  local items, page = st.items, st.page
  local first = (page - 1) * PER_PAGE + 1
  local last = math.min(first + PER_PAGE - 1, #items)
  local pages = math.max(1, math.ceil(#items / PER_PAGE))
  local lines = { string.format("\\y%s \\d(%d/%d)\n\n", st.title, page, pages) }
  local keys = 0
  for i = first, last do
    local n = i - first + 1
    lines[#lines + 1] = string.format("\\r%d. \\w%s\n", n, items[i][1])
    keys = keys | (1 << (n - 1))
  end
  lines[#lines + 1] = "\n"
  if page < pages then lines[#lines + 1] = "\\r8. \\wNext\n"; keys = keys | (1 << 7) end
  if page > 1 then lines[#lines + 1] = "\\r9. \\wBack\n"; keys = keys | (1 << 8) end
  lines[#lines + 1] = "\\r0. \\wExit"; keys = keys | (1 << 9)
  xmod.msg.show_menu(id, keys, st.timeout or -1, table.concat(lines))
end

function M.show(id, def)
  open[id] = { title = def.title or "Menu", items = def.items or {}, on_select = def.on_select, page = 1, timeout = def.timeout }
  render(id)
end
function M.close(id) if open[id] then open[id] = nil; xmod.msg.show_menu(id, 0, 0, " ") end end
function M.is_open(id) return open[id] ~= nil end

xmod.on("client_command", function(id, cmd, args)
  if cmd ~= "menuselect" then return end
  local st = open[id]; if not st then return end
  local key = tonumber(args[1]); if not key then return end
  if key == 0 then open[id] = nil return end
  if key == 8 then st.page = st.page + 1; render(id) return end
  if key == 9 then st.page = math.max(1, st.page - 1); render(id) return end
  local item = st.items[(st.page - 1) * PER_PAGE + key]
  if not item then return end
  open[id] = nil
  if st.on_select then st.on_select(id, item[1], item[2]) end
end)
xmod.on("client_disconnect", function(id) open[id] = nil end)
return M
