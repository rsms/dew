--[[
'dew' library (lib_dew.c) API:

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
    -h, --help       Print help and exit
    -v, --verbose    Log extra information
    --version        Print version and exit
    --debug-tokens   Print details about source tokenization
    --debug-parse    Print details about parsing
    --debug-resolve  Print details about identifier & type resolution
    --debug-codegen  Print details about code generation
    --debug-all      Print details about all phases
    --selftest[=v]   Run tests of dew itself (v=verbosely)
<input>: file path]]

function parse_args(args)
	local opt = {
		version = false,
		selftest = 0,
		args = {}
	}
	local i = 1
	while i <= #args do
		local arg = args[i]
		if arg == "-h" or arg == "--help" then
			print(fmt(USAGE, args[0]))
			os.exit(0)
		elseif arg == "--version" then
			print("dew version " .. VERSION)
			os.exit(0)
		elseif arg == "--debug-tokens" then
			DEBUG_TOKENS = true
		elseif arg == "--debug-parse" then
			DEBUG_PARSER = true
		elseif arg == "--debug-codegen" then
			DEBUG_CODEGEN = true
		elseif arg == "--debug-resolve" then
			DEBUG_RESOLVE = true
		elseif arg == "--selftest" then
			opt.selftest = 1
		elseif arg == "--selftest=v" then
			opt.selftest = 2
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

function selftest(verbose)
	print("intscan_test: start")
	require("intscan_test"){ verbose = verbose }
	print("intconv_test: start")
	require("intconv_test"){ verbose = verbose }
end

function compile_and_run(unit)
	-- tokenize
	tokens = tokenize_unit(unit)

	-- parse
	parse_unit(unit, tokens)
	if unit.errcount > 0 then
		dlog("stopping after %d error(s)", unit.errcount)
		return os.exit(1)
	end

	-- resolve types & identifiers
	resolve_unit(unit)
	if unit.errcount > 0 then
		dlog("stopping after %d error(s)", unit.errcount)
		return os.exit(1)
	end

	-- generate code
	code = codegen_unit(unit)
	if unit.errcount > 0 then
		dlog("stopping after %d error(s)", unit.errcount)
		return os.exit(1)
	end

	-- run program
	run(unit, code)
end

function main(args)
	local prog = args[0]
	local opt = parse_args(args)
	if opt.selftest > 0 then
		selftest(opt.selftest > 1)
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

-- main(arg)

local co_idgen = 0
local co_tab = {} -- thread => co table
-- TODO: replace linked-list queue with ring buffer or priority heap/queue
local co_runq_head = nil
local co_runq_tail = nil

function co_runq_enq(co)
	if co_runq_head == nil then
		co_runq_head = co
	else
		co_runq_tail.next = co
	end
	co_runq_tail = co
end

function co_runq_deq()
	local co = co_runq_head
	if co == nil then
		return nil
	end
	co_runq_head = co.next
	co.next = nil
	if co_runq_head == nil then
		co_runq_tail = nil
	end
	return co
end

function co_runq_remove(co)
	if co == co_runq_head then
		co_runq_head = co.next
		if co_runq_head == nil then
			co_runq_tail = nil
		end
		-- dlog("co_runq_remove(%s) head is now %s", co_id(co),
		--      co_runq_head == nil and "nil" or co_id(co_runq_head))
		co.next = nil
		return
	end

	local entry = co_runq_head
	while entry ~= nil do
		if entry.next == co then
			entry.next = co.next
			if co == co_runq_tail then
				co_runq_tail = entry
			end
			co.next = nil
			-- dlog("co_runq_remove(%s) tail is now %s", co_id(co),
			--      co_runq_tail == nil and "nil" or co_id(co_runq_tail))
			return
		end
		entry = entry.next
	end

	assert(false, fmt("%s not found in runq", co_id(co)))
end

function co_current()
	return co_tab[select(1, coroutine.running())]
end

function co_id(co)
	return "\x1b[1;3" .. (1 + (co.id % 6)) .. "m#" .. co.id .. "\x1b[0m"
	-- return "co:" .. co.id
end

function co_log1(co, format, ...)
	dlog("[" .. co_id(co) .. "] " .. format, ...)
end

function co_log(format, ...)
	return co_log1(co_current(), format, ...)
end

function co_terminate(co)
	co_log("terminating %s since parent %s exited", co_id(co), co_id(co.parent))
	co_runq_remove(co)

	-- co.error = "parent exited"
	-- local ok, err = coroutine.resume(co.f)
	-- return co_finalize(co, err)

	coroutine.close(co.f)
	return co_finalize(co, "parent exited")
end

function co_finalize(co, err)
	if err ~= nil then
		co_log1(co, "exited with error: %s\n%s", err, debug.traceback(co.f, err))
	else
		co_log1(co, "exited cleanly")
	end
	if co.parent == nil then
		-- main coroutine
		co_log("finalizing main %s", co_id(co))
	else
		co_log("finalizing %s (parent %s)", co_id(co), co.parent == nil and "-" or co_id(co.parent))
		assert(coroutine.status(co.f) == "dead", coroutine.status(co.f))
		if co.parent ~= nil and co.parent.children ~= nil then
			co.parent.children[co] = nil
		end
	end
	if co.children ~= nil then
		for co in pairs(co.children) do
			co_terminate(co)
		end
		co.children = nil
	end
	co_tab[co.f] = nil
	return co.parent == nil and true or nil -- 'true' for main, 'nil' for any other coroutine
end

function co_suspend(...) -- returns values passed to co_resume
	co_log("suspend")

	-- TODO: a "real" impl would not put co on run queue
	-- until whatever condition it's waiting for occurred
	co_runq_enq(co_current())

	if co_current().id == 0 then
		-- Main coroutine cannot be suspended (since it's technically not a coroutine.)
		-- Instead we resume the next coroutine on the run queue
		while true do
			local n = 0
			local co = co_runq_deq()
			if co == nil then
				-- empty run queue
				print("empty run queue")
				return
			elseif co.id == 0 then
				-- main coroutine
				return
			end
			return co_resume(co)
		end
	end

	-- local x, y, z = coroutine.yield(...)
	-- local err = co_current().error
	-- if err ~= nil then
	-- 	co_current().error = nil
	-- 	error(err)
	-- 	-- coroutine.close(co_current().f)
	-- 	-- co_finalize(co_current().f)
	-- end
	-- return x, y, z

	return coroutine.yield(...)
end

function co_resume(co, ...) -- (is_alive bool)
	co_log("resume %s", co_id(co))
	-- 'resume' returns either (true, ret1, ret2 ...) or (false, error)
	co.wait = 0
	local ok, s = coroutine.resume(co.f, ...)
	co_log1(co, "%s (yield=%s)", coroutine.status(co.f), tostring(s))
	-- our internal contract for coroutine main functions is that they return nil on completion
	-- and anything else when yielding.
	assert(not ok or (s == nil) == (coroutine.status(co.f) == "dead"))
	if ok and s ~= nil then
		-- still alive (suspended)
		return true
	end
	return co_finalize(co, s)
end

local WAIT_INBOX = 1 -- waiting for message in inbox

function co_spawn(f)
	co_idgen = co_idgen + 1
	local co = {
		f = coroutine.create(f),
		id = co_idgen,
		parent = co_current(),
		waiting = 0, -- WAIT_
	}
	co_tab[co.f] = co
	co_log("spawn %s (co %p) (f %p)", co_id(co), co, co.f)
	local ok, s = coroutine.resume(co.f)
	co_log1(co, "%s (yield=%s)", coroutine.status(co.f), tostring(s))
	if ok and s ~= nil then
		-- child suspended, waiting for something
		if co.parent.children == nil then
			co.parent.children = {}
		end
		co.parent.children[co] = 1
		return co
	end
	return co_finalize(co, s)
end

function co_sleep(ms)
	return co_suspend(123)
end

function co_timer(ms)
	return 1
end

function co_send(dest_co, msg)
	if dest_co.wait == WAIT_INBOX then
		co_log("send to %s (immediately)", co_id(dest_co))
		co_runq_remove(dest_co)
		return co_resume(dest_co, co_current(), msg)
	end
	if dest_co.inbox == nil then
		dest_co.inbox = {}
	end
	dest_co.inbox[#dest_co.inbox + 1] = { co_current(), msg }
	co_log("send to %s (inbox)", co_id(dest_co))
	return co_suspend(123)
end

function co_recv(ms)
	local co = co_current()
	if co.inbox ~= nil then
		local m
		for i = 1, #co.inbox do
			m = co.inbox[i]
			if m ~= nil then
				co.inbox[i] = nil
				return m[1], m[2]
			end
		end
	end
	co_log("recv: suspending until there are messages in inbox")
	co.wait = WAIT_INBOX
	return co_suspend(123)
end

function co_wait(co)
	return co_suspend(456)
end

function main_sched_test(args)
	--[[
	Things I'd like to do with coroutines
	  - wait for some coroutines to finish ("join", WaitGroup in Go)
	  - receive a message (e.g. progress update, workload, co existed, co error'd, ...)
	  - hand off ownership of a child co to another co (e.g. "detach" or "re-parent")
	  - ability to cancel blocking operations (eg read() with timeout)
	  - Run some code when exiting because a parent is exiting
	        Maybe use the "recover" pattern of Go. It's a bit akward tho...
	        	defer fun() {
					if err = recover() {
						print("returning because of error", err)
					}
	        	}
	]]

	--[[-- message passing
	A = co_spawn(function()
		local sender, msg = co_recv()
		print("child got message '"..msg.."' from "..co_id(sender))
	end)
	co_send(A, "hello")
	co_recv() -- wait for exit signal from child]]

	-- (interval_usec, leeway_usec, callback)
	local timer = dew.runloop_add_timer(dew.time()+800000000, 0, function()
		print("dew.runloop_add_timer callback")
	end)
	dlog("dew.runloop_add_timer => #%d", timer)

	-- local n = 4
	-- local timer = dew.runloop_add_repeating_timer(400000000, 10000000, function()
	-- 	print("dew.runloop_add_repeating_timer callback")
	-- 	if n == 0 then dew.runloop_add_cancel(timer) end
	-- 	n = n - 1
	-- end)
	-- dlog("dew.runloop_add_repeating_timer => #%d", timer)

	while dew.runloop_process() do
		dlog("dew.runloop_process() tick")
	end
	dlog("dew.runloop_process() has no work; exiting")

	--[[-- structured concurrency: child terminated when parent exits
	co_spawn(function()
		pcall(co_sleep, 1)
		print("sleep finished") -- never gets here
	end)
	return -- child terminated here before sleep finish]]

	--[[local A = co_spawn(function()
		print("[#1] enter")
		local B = co_spawn(function()
			print("[#2] enter")
			local C = co_spawn(function()
			   print("[#3] enter")
			   -- error("[#3] some error")
			   print("[#3] exit")
			end)
			print("[#2] #3 =", C) -- nil since C exited immediately
			print("[#2] co_sleep(100)"); co_sleep(100)
			print("[#2] exit")
		end)
		print("[#1] #2 =", B)
		-- print("[#1] timer(100)"); timer(100); co_recv()
		-- error("some error")
		co_wait(B) -- wait here until B exits
		-- TODO: maybe better to do 'wait' with channels? Or erlang style with signals?
		print("[#1] exit")
	end)
	print("[#0] #1 =", A)]]
end


-- setup main coroutine
local co = {
	f = select(1, coroutine.running()),
	id = 0,
	parent = nil,
	wait = 0,
}
co_tab[co.f] = co
if xpcall(main_sched_test, function(err, b, c)
	print("xpcall", err, b, c)
	return co_finalize(co, err)
end, arg) then
	return co_finalize(co)
end

-- -- run detached coroutines until run queue is empty
-- while true do
-- 	local n = 0
-- 	local co = co_runq_deq()
-- 	if co == nil then
-- 		break
-- 	end
-- 	co_resume(co)
-- end
