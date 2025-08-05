local csvs = {
	{ '../../wadsrc/static/language.0',         'lang_0.txt' },
	{ '../../wadsrc/static/language.csv',       'lang.txt' },
	{ '../../wadsrc_extra/static/language.csv', 'lang_extra.txt' },
--	{ '../../wadsrc_extra/static/filter/chex.chex3/language.csv',            'lang_chex3.txt' },
--	{ '../../wadsrc_extra/static/filter/hacx.hacx1/after_iwad/language.csv', 'lang_hacx1_after.txt' },
	{ '../../wadsrc_extra/static/filter/harmony/language.csv',               'lang_harmony.txt' },
}

local lang = 'chs'

local function loadCsv(s)
	local i, j, q, t, r = 1, 1, 0, {}, {}
	while true do
		local b = s:byte(j)
		j = j + 1
		if b == 0x22 then -- 0x22:'"'
			if q ~= 0 then
				q = 3 - q
			elseif i + 1 == j then
				i, q = j, 1
			end
		elseif q ~= 1 and (b == 0x2c or b == 13 or b == 10) or not b then -- 0x2c:','
			if b == 0x2c or i + 1 < j or #t > 0 then
				local c = s:sub(i, j - (q == 2 and s:byte(j - 2) == 0x22 and 3 or 2))
				if q == 2 then
					c = c:gsub('""', '"')
				end
				t[#t + 1] = c
				if b ~= 0x2c then
					r[#r + 1] = t
					t = {}
				end
			end
			if not b then
				return r
			end
			i, q = j, 0
		end
	end
end

local function saveCsv(lines)
	local t = {}
	for _, line in ipairs(lines) do
		for i, s in ipairs(line) do
			if i > 1 then
				t[#t + 1] = ','
			end
			if s:find '[",\r\n]' then -- if s:find '[,\r\n]' or s:find '^"' then
				t[#t + 1] = '"'
				t[#t + 1] = s:gsub('"', '""'):gsub('\r*\n', '\r\n')
				t[#t + 1] = '"'
			else
				t[#t + 1] = s
			end
		end
		t[#t + 1] = '\r\n'
	end
	if #t > 0 then t[#t] = nil end -- remove last '\r\n'
	return table.concat(t)
end

for _, p in ipairs(csvs) do
	print(p[1] .. ' + ' .. p[2] .. ' => ' .. p[1])
	local f = io.open(p[1], 'rb')
	local s = f:read '*a'
	f:close()
	local lines = loadCsv(s)
	local colId = 0
	for i, s in ipairs(lines[1]) do
		if s == lang then
			colId = i
			break
		end
	end
	if colId == 0 then
		colId = #lines[1] + 1
		lines[1][colId] = lang
	end

	local t, tt = {}, {}
	local k, v, e
	local i = 0
	for line in io.lines(p[2]) do
		i = i + 1
		if not k then
			k = line:match '^> (.+)$'
		elseif line:find '^; ' then
		elseif line == '' then
			if v and v:find '^"""' then
				v = v .. '\r\n'
			else
				if k then
					if t[k] then
						print('WARN: duplicated id: "' .. k .. '"')
					end
					if e and not v then
						print('WARN: not translated id: "' .. k .. '"')
					end
					t[k] = {e or '', v}
					if e and v then
						if tt[e] and tt[e] ~= v then
							print('WARN: unmatched translation: "' .. e .. '"\n' .. tt[e] .. '\n' .. v)
						else
							tt[e] = v
						end
					end
				end
				k, e, v = nil, nil, nil
			end
		else
			if not v then
				if not k then
					print('WARN: found extra line @ ' .. i)
				end
				v = line
			else
				v = v .. '\r\n' .. line
			end
			v = v:gsub('^"""(.-)"""', '%1')
			if not v:find '^"""' then
				if not e then
					e = v
					v = nil
				else
					if t[k] then
						print('WARN: duplicated id: "' .. k .. '"')
					end
					t[k] = {e, v}
					if tt[e] and tt[e] ~= v then
						print('WARN: unmatched translation: "' .. e .. '"\n' .. tt[e] .. '\n' .. v)
					else
						tt[e] = v
					end
					k, e, v = nil, nil, nil
				end
			end
		end
	end

	for i, line in ipairs(lines) do
		if i > 1 then
			local en = line[1] or '' -- default
			local id = line[2] or '' -- Identifier
			if line[4] and line[4] ~= '' then
				id = id .. ' ' .. line[4] -- Filter
			end
			id = id:gsub('\r', ''):gsub('\n', '\\n')
			if id ~= '' then
				if t[id] then
					if t[id][1] ~= en then
						print('WARN: unmatched default: "' .. id .. '":\n"""' .. en .. '"""\n"""' .. (t[id][1] or '<nil>') .. '"""')
					end
				else
					print('WARN: not found id: "' .. id .. '"')
				end
			end
			line[colId] = id and t[id] and t[id][2] or line[colId] or ''
		end
	end
	s = saveCsv(lines)
	local f = io.open(p[1], 'wb')
	f:write(s)
	f:close()
end
print 'done!'
