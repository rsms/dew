--[[
'runtime' library exported as '__rt' implemented in runtime.c:

type error int
fun errstr(e error) (name, description str)
fun intscan(s str, base int = 10, limit u64 = U64_MAX, isneg bool = false) (u64, error)
fun intfmt(value i64, base int, is_unsigned bool = false) (str, error)
fun intconv(value i64, src_bits, dst_bits uint, src_issigned, dst_issigned bool) int

// error constants
const ERR_OK
const ERR_INVALID
const ERR_RANGE
const ERR_INPUT
const ERR_...

]]

require("util")
require("dew_debug")
require("hash")
require("unit")
require("diag")
require("srcpos")
require("tokenize")
require("id")
require("ast")
require("parse")
require("resolve")
require("codegen")

VERSION = "0.1"
DEBUG_TOKENS = false
DEBUG_PARSER = false
DEBUG_RESOLVE = false
DEBUG_CODEGEN = false
VERBOSE = 1 -- 0=quiet, 1=normal, 2=verbose


---------------------------------------------------------------------------------------------------
-- run
---------------------------------------------------------------------------------------------------


function run(unit, code)
	print("—— run")
	local env = _ENV
	local func, err = load(code, unit.srcfile, "t", env)
	if not func then
		print("Compile error: " .. err)
	else
		print(func())
	end
end


---------------------------------------------------------------------------------------------------
-- main
---------------------------------------------------------------------------------------------------


USAGE = [[
usage: %s [options] <input> ...
options:
    -h, --help          Print help and exit
    -v, --verbose       Log extra information
    --version           Print version and exit
    --debug-tokens      Print details about source tokenization
    --debug-parse       Print details about parsing
    --debug-resolve     Print details about identifier & type resolution
    --debug-codegen     Print details about code generation
    --debug-all         Print details about all phases
    --selftest[=filter] Run tests of dew itself
    --selftest-verbose  Print details about selftest
<input>: file path]]

function parse_args(args)
	local opt = {
		version = false,
		selftest = nil, -- string?
		selftest_verbose = false,
		args = {}
	}
	local i = 1
	while i <= #args do
		local arg = args[i]
		if arg == "-h" or arg == "--help" then
			printf(USAGE, args[0])
			os.exit(0)
		elseif arg == "--version" then
			print("dew version " .. VERSION)
			os.exit(0)
		elseif arg == "--debug-tokens" then
			DEBUG_TOKENS = true
		elseif arg == "--debug-parse" then
			DEBUG_PARSER = true
		elseif arg == "--debug-resolve" then
			DEBUG_RESOLVE = true
		elseif arg == "--debug-codegen" then
			DEBUG_CODEGEN = true
		elseif arg == "--selftest" then
			opt.selftest = ""
		elseif string_starts_with(arg, "--selftest=") then
			opt.selftest = arg:sub(#"--selftest=" + 1)
		elseif arg == "--selftest-verbose" then
			opt.selftest_verbose = true
		elseif arg == "--debug-all" then
			DEBUG_TOKENS = true
			DEBUG_PARSER = true
			DEBUG_CODEGEN = true
			DEBUG_RESOLVE = true
		elseif arg == "-v" or arg == "--verbose" then
			VERBOSE = 2
		elseif arg == "-" or not arg:match("^%-") then
			table.insert(opt.args, arg)
		else
			print("Unknown option: " .. arg)
			os.exit(1)
		end
		i = i + 1
	end
	return opt
end

function compile_and_run(unit)
	local function stop_if_errors()
		if unit.errcount > 0 then
			dlog("stopping after %d error(s)", unit.errcount)
			return os.exit(1)
		end
	end

	-- tokenize (text -> tokens)
	tokens = tokenize_unit(unit)
	stop_if_errors()

	-- parse (tokens -> AST)
	parse_unit(unit, tokens, PARSE_SRCMAP)
	stop_if_errors()

	-- resolve (AST -> IR)
	resolve_unit(unit)
	stop_if_errors()

    print("TODO: ... os.exit(1)"); os.exit(1)

	-- generate code (IR -> target)
	code = codegen_unit(unit)
	stop_if_errors()

	-- run program
	run(unit, code)
end

function main(args)
	local prog = args[0]
	local opt = parse_args(args)
	if opt.selftest ~= nil then
		require("selftest")(opt.selftest, opt.selftest_verbose)
		os.exit(0)
	end
	if #opt.args == 0 then
		io.stderr:write(fmt("%s: missing <input>\n", prog))
		os.exit(1)
	end
	local unit, code
	for i, filename in ipairs(opt.args) do
		-- load source file
		if filename == "-" then
			unit = unit_create_buf("<stdin>", io.read("*a"))
		else
			unit = unit_create_file(filename)
		end
		compile_and_run(unit)
	end
end

-- -- Runtime dev
-- require("tests/rt/worker-echo")
-- require("ast-lab")

if false then
	dew_debug_calltrace_enable()
	xpcall(main, function(err)
		debug.sethook()
		print("[dew_debug] error: " .. debug.traceback(err, 2))
		dew_debug_print_calltrace(20)
		os.exit(3)
	end, arg)
else
	return main(arg)
end
