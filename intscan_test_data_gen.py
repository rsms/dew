import itertools

data_file = "intscan_test_data.csv"
bases = range(2, 37)
nbitss = range(2, 65)
DIGITS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"

def generate_limits(nbits):
	"""Generate maximum unsigned, maximum signed, and minimum signed values for a given bit size."""
	max_unsigned = (1 << nbits) - 1
	max_signed = (1 << (nbits - 1)) - 1
	min_signed = -(1 << (nbits - 1))
	return max_unsigned, max_signed, min_signed

def encode_in_base(value, base):
	"""Encode an integer value as a string in the given base."""
	if value < 0:
		return "-" + encode_in_base(-value, base)
	encoded = ""
	while value > 0:
		encoded = DIGITS[value % base] + encoded
		value //= base
	return encoded if encoded else "0"

def insert_underscores(s):
	if len(s) <= 2:
		return s
	if len(s) == 3:
		return s[0] + '_' + s[1:]
	# Reverse the string, insert underscores every 3 characters, and reverse back
	reversed_s = s[::-1]
	chunks = [reversed_s[i:i+3] for i in range(0, len(reversed_s), 3)]
	return '_'.join(chunks)[::-1]

def generate_test_data(flavor):
	L = []
	if flavor == 'OK':
		L.append(('nbits','base','is_signed','input','expected_value','comment'))
		for nbits, base in itertools.product(nbitss, bases):
			max_unsigned, max_signed, min_signed = generate_limits(nbits)
			smax_str = encode_in_base(max_signed, base)
			smin_str = encode_in_base(min_signed, base)
			umax_str = encode_in_base(max_unsigned, base)

			L.append((nbits, base, 1, "000", 0, "zero"))

			L.append((nbits, base, 1, encode_in_base(max_signed//2, base), max_signed//2,
			          ("S%d_MAX/2" % nbits)))
			L.append((nbits, base, 1, encode_in_base(min_signed//2, base), min_signed//2,
			          ("S%d_MIN/2" % nbits)))
			L.append((nbits, base, 0, encode_in_base(max_unsigned//2, base), max_unsigned//2,
			          ("U%d_MAX/2" % nbits)))

			L.append((nbits, base, 1, smax_str, max_signed,   ("S%d_MAX" % nbits)))
			L.append((nbits, base, 1, smin_str, min_signed,   ("S%d_MIN" % nbits)))
			L.append((nbits, base, 0, umax_str, max_unsigned, ("U%d_MAX" % nbits)))

			L.append((nbits, base, 1, "000" + smax_str, max_signed,
			          ("S%d_MAX with leading zeroes" % nbits)))
			L.append((nbits, base, 1, "-000" + smin_str[1:], min_signed,
			          ("S%d_MIN with leading zeroes" % nbits)))
			L.append((nbits, base, 0, "000" + insert_underscores(umax_str), max_unsigned,
			          ("U%d_MAX with leading zeroes" % nbits)))

			if len(smax_str) > 2:
				L.append((nbits, base, 1, insert_underscores(smax_str), max_signed,
				          ("S%d_MAX with '_'" % nbits)))
			if len(smin_str) > 3:
				L.append((nbits, base, 1, "-"+insert_underscores(smin_str[1:]), min_signed,
				          ("S%d_MIN with '_'" % nbits)))
			if len(umax_str) > 2:
				L.append((nbits, base, 0, insert_underscores(umax_str), max_unsigned,
				          ("U%d_MAX with '_'" % nbits)))
	elif flavor == 'ERR_RANGE':
		L.append(('nbits','base','is_signed','input','comment'))
		for nbits, base in itertools.product(nbitss, bases):
			max_unsigned, max_signed, min_signed = generate_limits(nbits)
			L.append((nbits, base, 1, encode_in_base(max_signed + 1, base),
			          ("S%d_MAX+1" % nbits)))
			L.append((nbits, base, 1, encode_in_base(min_signed - 1, base),
			          ("S%d_MIN-1" % nbits)))
			L.append((nbits, base, 0, encode_in_base(max_unsigned + 1, base),
			          ("U%d_MAX+1" % nbits)))
	elif flavor == 'ERR_INPUT':
		L.append(('nbits','base','is_signed','input','comment'))
		# input that should cover all cases as it's unaffected by variables
		L.append((nbitss[0], bases[0], 0, "", "empty"))
		L.append((nbitss[0], bases[0], 0, "_", "invalid '_'"))
		for nbits, base in itertools.product(nbitss, bases):
			# Generate invalid leading and trailing underscores
			s = encode_in_base((1 << nbits) - 1, base)
			if len(s) > 1:
				L.append((nbits, base, 0, "_" + s, "invalid leading '_'"))
				L.append((nbits, base, 0, s + "_", "invalid trailing '_'"))

			c = DIGITS[base] if base < 36 else '@'
			if len(s) == 1:
				L.append((nbits, base, 0, c, f"'{c}' outside of base encoding"))
			else:
				L.append((nbits, base, 0, c + s[1:], f"'{c}' outside of base encoding"))
				L.append((nbits, base, 0, s[:-1] + c, f"'{c}' outside of base encoding"))
				if len(s) > 2:
					L.append((nbits, base, 0, s[:len(s)//2] + c + s[len(s)//2 + 1:],
					          f"'{c}' outside of base encoding"))

	return L

def write_csv(filename, rows):
	"""Write test cases to a text file in tabular format."""
	with open(filename, "w") as f:
		for row in rows:
			f.write(",".join([str(col).replace(",","\\,") for col in row]) + "\n")
	print(f"write {filename}")

if __name__ == "__main__":
	write_csv("intscan_test_data_ok.csv", generate_test_data('OK'))
	write_csv("intscan_test_data_err_range.csv", generate_test_data('ERR_RANGE'))
	write_csv("intscan_test_data_err_input.csv", generate_test_data('ERR_INPUT'))
