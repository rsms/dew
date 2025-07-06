identifiers, identifier_names = {}, {}
function id_intern(name)
	local idx = identifier_names[name]
	if idx == nil then
		idx = #identifiers + 1
		-- Note: AST encoding of ID uses a uint24 to encode the identifier index
		if idx == 0xffffff then error("too many identifiers") end
		identifiers[idx] = name
		identifier_names[name] = idx
		-- dlog("+ID #%4d '%s'", idx, name)
	end
	return idx
end

function assert_id_valid(id_idx)
	assert(id_idx > 0 and id_idx <= #identifiers, fmt("invalid id_idx %d", id_idx))
end

function id_str(id_idx) return identifiers[id_idx] end

ID__ = id_intern("_")
