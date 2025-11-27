local csvs = {
	{ '../../wadsrc/static/language.0',         'lang_0.txt' },
	{ '../../wadsrc/static/language.csv',       'lang.txt' },
	{ '../../wadsrc_extra/static/language.csv', 'lang_extra.txt' },
	{ '../../wadsrc_extra/static/filter/chex.chex3/language.csv',            'lang_chex3.txt' },
	{ '../../wadsrc_extra/static/filter/hacx.hacx1/after_iwad/language.csv', 'lang_hacx1_after.txt' },
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

for _, p in ipairs(csvs) do
	print(p[1] .. ' => ' .. p[2])
	local f = io.open(p[1], 'rb')
	local s = f:read '*a'
	f:close()
	local lines = loadCsv(s)
	local colId = 0
	for i, s in ipairs(lines[1]) do
		if s == lang or (s == 'jp' or s:sub(1,2) == 'ja') and colId == 0 then
			colId = i
		end
	end

	local t = {}
	f = io.open(p[2], 'wb')
	for i, line in ipairs(lines) do
		if i > 1 then
			local en = line[1] or '' -- default
			local id = line[2] or '' -- Identifier
			local rm = line[3] or '' -- Remarks
			local cn = line[colId] or ''
			if line[4] and line[4] ~= '' then
				id = id .. ' ' .. line[4] -- Filter
			end
			id = id:gsub('\r', ''):gsub('\n', '\\n')
			if id ~= '' then
				if t[id] then
					print('WARN: duplicated id: "' .. id .. '"')
				end
				t[id] = true
				f:write('> ', id, '\n')
			end
			if rm ~= '' then f:write('; ', rm:gsub('\r', ''):gsub('^%s+', ''):gsub('%s+$', ''):gsub('\n', '\\n'), '\n') end
			if en ~= '' or cn ~= '' then
				if en:find '^"""' or en:find '^; ' or en:find '[\r\n]; ' then
					error('ERROR: invalid content in english: "' .. en .. '"')
				end
				if en:find '[\r\n]' or en == "" then
					f:write('"""', en:gsub('\r', ''), '"""\n')
				else
					f:write(en, '\n')
				end
				if cn ~= '' then
					if cn:find '^"""' or cn:find '^; ' then
						error('ERROR: invalid prefix: "' .. cn .. '"')
					end
					if cn:find '[\r\n]' then
						f:write('"""', cn:gsub('\r', ''), '"""\n')
					else
						f:write(cn, '\n')
					end
				else
					f:write '; NEED TRANSLATION\n'
				end
			end
			f:write '\n'
		end
	end
	f:close()
end
print 'done!'
