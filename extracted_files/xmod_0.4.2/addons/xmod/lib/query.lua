-- xmod query helper:  local Q = require("query")
-- Q.NULL                          SQL NULL inside parameter lists:  xmod.query.exec(db, "INSERT ... VALUES (?, ?)", { "name", Q.NULL }, cb)
-- Q.with(profile, fn, on_error)   open a profile, call fn(db, done); the connection is closed when you call done()
local Q = {}
Q.NULL = { ["$null"] = true }
function Q.with(profile, fn, on_error)
  xmod.query.open(profile, function(err, db)
    if err then
      if on_error then on_error(err) else xmod.error(("query: cannot open profile '%s': %s"):format(profile, err)) end
      return
    end
    local closed = false
    fn(db, function() if not closed then closed = true; xmod.query.close(db, function() end) end end)
  end)
end
return Q
