-- hash_mix64 "mixes" a 64-bit integer value, based on MurmurHash3's finalizer.
-- Provides excellent avalanche properties for individual hash values
function hash_mix64(h)
	h = h ~ (h >> 33)
	h = (h * 0xFF51AFD7ED558CCD) & 0xFFFFFFFFFFFFFFFF
	h = h ~ (h >> 33)
	h = (h * 0xC4CEB9FE1A85EC53) & 0xFFFFFFFFFFFFFFFF
	h = h ~ (h >> 33)
	return h
end

-- hash_combine64 combines two 64-bit hashes with good distribution.
-- Uses a variant of the FNV-1a approach with better constants
function hash_combine64(h1, h2)
    return hash_mix64(((h1 ~ (h2 << 1)) * 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF)
end
