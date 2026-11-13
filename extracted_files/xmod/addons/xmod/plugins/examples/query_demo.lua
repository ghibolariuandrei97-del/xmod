-- QUERY EXAMPLE: persistent player visit counter in SQLite (profile [db:main] in configs/query.ini) + a few other xmod.query calls.
-- Needs the xmod_query extension (metamod plugins.ini: xmod first, then xmod_query).  All calls are asynchronous: the game never waits.
xmod.register("Query demo", "0.1", "xmod")
local db                                             -- connection handle (integer), nil until opened


xmod.on("config", function()
  if db then return end
  if not xmod.ext.available("query") then xmod.error("query_demo: the xmod_query extension is not loaded"); return end
  xmod.query.open("main", function(err, h)
    if err then xmod.error("query_demo: " .. err); return end
    db = h
    xmod.query.exec(db, "CREATE TABLE IF NOT EXISTS visits(steamid TEXT PRIMARY KEY, name TEXT, visits INTEGER NOT NULL DEFAULT 0, last_seen INTEGER)", function(e)
      if e then xmod.error("query_demo: " .. e) else xmod.log("query_demo: database ready") end
    end)
  end)
end)

xmod.on("client_putinserver", function(id)
  if not db or xmod.player.is_bot(id) then return end
  local sid, name = xmod.player.steamid(id), xmod.player.name(id)
  -- upsert, then read the counter back. Both statements use the same handle, so they run in this order.
  xmod.query.exec(db, "INSERT INTO visits(steamid, name, visits, last_seen) VALUES (?, ?, 1, ?) ON CONFLICT(steamid) DO UPDATE SET visits = visits + 1, name = excluded.name, last_seen = excluded.last_seen",
    { sid, name, os.time() }, function(err) if err then xmod.error("visit upsert: " .. err) end end)
  xmod.query.exec(db, "SELECT visits FROM visits WHERE steamid = ?", { sid }, function(err, res)
    if err or not res.rows[1] then return end
    if xmod.player.connected(id) then                -- the player may have left while the query ran: always re-check in callbacks
      xmod.msg.chat(id, ("{g}[visits] {y}Welcome {t}%s{y}! This is visit number {g}%d{y}."):format(name, res.rows[1].visits))
    end
  end)
end)

-- say /top  -> top 5 visitors
xmod.cmd.say("/top", "", function(id)
  if not db then return end
  xmod.query.exec(db, "SELECT name, visits FROM visits ORDER BY visits DESC LIMIT 5", function(err, res)
    if err then return end
    for i, row in ipairs(res.rows) do if xmod.player.connected(id) then xmod.msg.chat(id, ("{g}#%d {t}%s {y}- %d visits"):format(i, row.name, row.visits)) end end
  end)
end)

-- other calls (uncomment and adapt; hosts must be allowed by http_allow / socket_allow in query.ini):
-- xmod.query.http_get("http://127.0.0.1:8080/status", { timeout = 3 }, function(err, r) if not err then xmod.log("status " .. r.status .. ": " .. r.body) end end)
-- xmod.query.http_post("https://api.example.com/hook", '{"event":"map"}', { headers = { ["Content-Type"] = "application/json" } }, function(err, r) end)
-- xmod.query.udp_request("127.0.0.1", 27015, "\xff\xff\xff\xffTSource Engine Query\0", { timeout = 2 }, function(err, r) end)   -- A2S_INFO
-- xmod.query.tcp_request("127.0.0.1", 6379, "PING\r\n", { read_until = "\n", timeout = 1 }, function(err, r) end)                 -- line protocol
