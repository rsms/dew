import { WASI } from "./wasi/wasi"
import * as WASISnapshotPreview1 from "./wasi/snapshot-preview1"

const DEW_LUA = `
local f, err = io.open("dew.lua")
if err ~= nil then error(err) end
local src = f:read("a")
f:close()
print("src:", src)

--function a()
--	print("a> enter")
--	error("ERROR A")
--	print("a> return")
--end
--function b()
--	print("b> enter")
--	local ok, err = pcall(a)
--	print("b> pcall(a) => ", ok, err)
--	error("ERROR B")
--	print("b> return")
--end
--function c()
--	print("c> enter")
--	local ok, err = pcall(b)
--	print("c> pcall(b) => ", ok, err)
--	print("c> return")
--end
--pcall(c)

print(table.unpack(undef))

--os.exit(123)
`

// int align2<T>(n: int, w: int) rounds up n to closest boundary w (w must be a power of two)
function align2(n, w) {
	return ((n >>> 0) + (w - 1)) & ~(w - 1)
}


class ProcExit extends Error {
	constructor(status) {
		super()
		this.status = status
	}
}


class ByteBuf {
	constructor(initsize, growstep) {
		this.buf = new Uint8Array(initsize)
		this.len = 0
		this.growstep = growstep ? align2(growstep, 8) : 8
	}
	get cap() { return this.buf.length }
	indexOf(byte) {
		return this.buf.subarray(0, this.len).indexOf(byte)
	}
	slice(begin, end) {
		return this.buf.subarray(begin, end)
	}
	reserve(nbytes) {
		const needcap = this.len + nbytes
		if (needcap <= this.cap)
			return
		const newcap = align2(needcap, this.growstep)
		// dlog("ByteBuf grow", this.cap, "->", newcap)
		const newbuf = new Uint8Array(newcap)
		newbuf.set(this.buf.subarray(0, this.buflen))
		this.buf = newbuf
	}
	append(chunk) {
		this.reserve(chunk.length)
		this.buf.set(chunk, this.len)
		this.len += chunk.length
	}
}


class LineBuf {
	constructor(lineHandler) {
		this.lineHandler = lineHandler
		this.buf = new ByteBuf(8, 64)
	}
	close() {
		let line
		let len = this.buf.len
		while (len > 0) {
			let buf = this.buf.slice(0, len)
			try {
				line = new TextDecoder().decode(buf)
			} catch(_) {
				len--
				continue
			}
			return this.lineHandler(line)
		}
	}
	write(chunk) {
		this.buf.append(chunk)
		let begin = 0, end = 0
		const bufslice = this.buf.slice(0, this.buf.len)
		while (begin < this.buf.len) {
			end = bufslice.indexOf(0x0A, begin)
			if (end == -1) {
				if (begin == 0)
					return
				break
			}
			let line = this.buf.slice(begin, end)
			this.lineHandler(new TextDecoder().decode(line))
			begin = end + 1
		}
		if (this.buf.len == begin) {
			this.buf.len = 0
		} else {
			// dlog(`LineBuf: remaining: ${new TextDecoder().decode(this.buf.slice(begin))}`)
			// dlog(`LineBuf: copyWithin(${0}, ${begin})`)
			this.buf.buf.copyWithin(0, begin)
			this.buf.len -= begin
		}
	}
}


export const syscalls = {}


export class Runtime {
	constructor() {
		this.module = null
		this.instance = null
		this.memory = null
		this.mem_u8 = null
		this.mem_i32 = null
		this.mem_u32 = null
		this.suspended = false
		this.suspend_data_addr = 0
		this.suspend_stack_size = 0
		this.suspend_result = null // resumed return value
		this.done = new Promise((resolve, reject) => {
			this.doneResolve = resolve
			this.doneReject = reject
		})
		this.onStdoutLine = line => console.log("stdout>", line)
		this.onStderrLine = line => console.warn("stdout>", line)

		this.stdout = new LineBuf(line => this.onStdoutLine(line))
		this.stderr = new LineBuf(line => this.onStderrLine(line))

		this.wasi = null

		// // open(fdDir: FileDescriptor, path: WASIPath, oflags: number, fdflags: number)
		// const ROOT_DIR_FD = 3 // see WASIDrive.constructor
		// const OFLAGS = WASISnapshotPreview1.OpenFlags
		// const [err, fd] = this.wasi.drive.open(ROOT_DIR_FD, "a.txt", OFLAGS.CREAT | OFLAGS.TRUNC, 0)
		// console.log("open", {err, fd})
		// const wres = this.wasi.drive.write(fd, new TextEncoder().encode(DEW_LUA))
		// console.log("write", wres)
		// this.wasi.drive.close(fd)
	}

	u8(ptr)  { return this.mem_u8[ptr] }

	i32(ptr) { return this.mem_i32[ptr >>> 2] }
	u32(ptr) { return this.mem_u32[ptr >>> 2] >>> 0 }

	setI32(ptr, v) { this.mem_u32[ptr >>> 2] = v }
	setU32(ptr, v) { this.mem_u32[ptr >>> 2] = (v >>> 0) }

	// u64(ptr) { BigInt ... }

	// if (WASM32)
	isize(ptr) { return this.mem_i32[ptr >>> 2] }
	usize(ptr) { return this.mem_u32[ptr >>> 2] >>> 0 }

	async start(fetchPromise, prog) {
		const rt = this

		const now = new Date()
		this.wasi = new WASI(/*WASIContextOptions*/{
			args: ["dew"],
			// args: ["dew", "/input.dew"],
			env: {}, // {string:string}
			stdout: chunk => this.stdout.write(chunk),
			stderr: chunk => this.stderr.write(chunk),
			// stdout: (out) => console.log("stdout> " + out),
			// stderr: (err) => console.error("stderr> " + err),
			// stdin: () => {
			// 	console.log("read stdin")
			// 	return prog
			// },
			// debug(){}, // DebugFn
			isTTY: false, // boolean
			fs: {
				// "/dew.lua": {
				// 	path: "/dew.lua",
				// 	timestamps: { access: now, change: now, modification: now },
				// 	// mode: "string", content: DEW_LUA,
				// 	content: new TextEncoder().encode(DEW_LUA),
				// },
				"/input.dew": {
					path: "/input.dew",
					timestamps: { access: now, change: now, modification: now },
					// mode: "string", content: DEW_LUA,
					content: new TextEncoder().encode(prog),
				},
			},
		})
		console.log("wasi instance:", this.wasi)

		const imports = this.wasi.getImportObject() // returns a new object that we own
		imports.wasi_snapshot_preview1.proc_exit = (status) => {
			throw new ProcExit(status)
		}

		// const memory = new WebAssembly.Memory({ initial: 32, maximum: 65536 })

		function syscall(op, ...args) {
			rt.updateMemoryViewsIfChanged()
			if (rt.suspended)
				return rt.finalizeResume()
			// dlog("syscall", {op}, ...args)
			const f = syscalls[op]
			if (f)
				return f(rt, ...args)
			console.warn("[dew] unsupported syscall", op)
			return -ENOSYS;
		}
		imports.env = {
			// memory, // import memory

			// syscall is the FFI boundary.
			// Because of wasm-js FFI constraints, we use different functions depending on arg types:
			//   i = i32 in wasm, represented as integer number in JS
			//   I = i64 in wasm, represented as BigNum in JS
			//   f = f64 in wasm, represented as float number in JS
			// These are all handled by the same JS function since each individual syscall
			// implementation knows what types of arguments to expect.
			syscall:   syscall,
			syscall_I: syscall,
			syscall_f: syscall,

			wlongjmp_scope(block_ptr) {
				try {
					rt.instance.exports.exec_block(block_ptr)
					return 0
				} catch (arg) {
					return arg
				}
			},

			wlongjmp(arg) {
				throw arg
			},
		}

		const { module, instance } = await WebAssembly.instantiateStreaming(fetchPromise, imports)
		console.log("Runtime.instance", instance)
		console.log("Runtime.instance exports", {...instance.exports})

		this.module = module
		this.instance = instance
		// this.memory = instance.exports.memory || memory
		this.memory = instance.exports.memory || memory
		this.memorySize = this.memory.buffer.byteLength

		this.wasi.hasBeenInitialized = true
		this.wasi.instance = this.instance
		this.wasi.module = this.module
		this.wasi.memory = this.memory

		this.updateMemoryViews()

		// Get address of pre-allocated memory for asyncify suspension
		this.suspend_data_addr = this.instance.exports.asyncify_data.value
		// console.log(`suspend_data 0x${this.suspend_data_addr} ` +
		//             `{ ${this.u32(this.suspend_data_addr)}, ${this.u32(this.suspend_data_addr + 4)} }`)
		// // See struct at https://github.com/WebAssembly/binaryen/blob/
		// // fd8b2bd43d73cf1976426e60c22c5261fa343510/src/passes/Asyncify.cpp#L106-L120
		// this.suspend_data_addr = this.instance.exports.suspend_data.value
		// this.suspend_data_size = this.u32(this.instance.exports.suspend_data_size.value)
		// this.setU32(this.suspend_data_addr, this.suspend_data_addr + 8)
		// this.setU32(this.suspend_data_addr + 4, this.suspend_data_addr + this.suspend_data_size)

		// this.dumpmem(this.suspend_data_addr, 8 + this.suspend_data_size)

		return this.resume1()
	}

	updateMemoryViewsIfChanged() {
		const memorySize = this.memory.buffer.byteLength
		if (this.memorySize == memorySize)
			return
		dlog(`wasm memory grown ${this.memorySize/1024} kiB -> ${memorySize/1024} kiB`)
		this.memorySize = memorySize
		this.updateMemoryViews()
	}

	updateMemoryViews() {
		this.mem_u8 = new Uint8Array(this.memory.buffer)
		this.mem_i32 = new Int32Array(this.memory.buffer)
		this.mem_u32 = new Uint32Array(this.memory.buffer)
	}

	dumpmem(addr, len) {
		function fmthex(uint8Array) {
			return Array.from(uint8Array)
				.map((byte, i) =>
				     (i%16 == 0 ? "\n" + (addr + i).toString(16).padStart(8, "0") + " │ " : " ") +
				     byte.toString(16).padStart(2, "0") )
				.join("")
		}
		console.log(`dumpmem 0x${addr}…0x${addr+len} (${len})` +
		            fmthex(this.mem_u8.subarray(addr, addr+len)))
	}

	// 0 = normal, 1 = unwinding, 2 = rewinding
	suspendState() { return this.instance.exports.asyncify_get_state() }

	suspend() {
		// dlog("suspend: suspendState() =", this.suspendState())
		assert(this.suspendState() == 0) // must not be suspended
		// Fill in the data structure. The first value has the stack location,
		// which for simplicity can start right after the data structure itself

		// const mem_start = this.u32(this.suspend_data_addr)
		// const mem_end = this.u32(this.suspend_data_addr + 4)
		// this.dumpmem(mem_start, mem_end - mem_start)

		// this.mem_i32[this.suspend_data_addr >> 2] = this.suspend_data_addr + 8
		// this.mem_i32[(this.suspend_data_addr + 4) >> 2] = this.suspend_stack_size // end of stack
		this.instance.exports.asyncify_start_unwind(this.suspend_data_addr)
		this.suspended = true

		// this.dumpmem(mem_start, mem_end - mem_start)

		return true
	}

	resume(result) {
		// dlog("resume: suspendState() =", this.suspendState())
		assert(this.suspendState() == 1) // must be unwinding
		assert(this.suspended == true)
		this.suspend_result = result
		this.instance.exports.asyncify_start_rewind(this.suspend_data_addr)
		// The code is now ready to rewind; to start the process, enter the
		// first function that should be on the call stack.
		return this.resume1()
	}

	finalizeResume() {
		// dlog("finalizeResume")
		assert(this.suspendState() == 2) // must be rewinding
		this.instance.exports.asyncify_stop_rewind()
		this.suspended = false
		return this.suspend_result
	}

	resume1() {
		try {
			const status = this.instance.exports._start()
			const suspendState = this.suspendState()
			// dlog("resume1: _start => suspendState =", suspendState)
			if (suspendState == 0)
				this.onExit(status === undefined ? 0 : status)
		} catch (err) {
			dlog("resume1: caught error", err)
			this.suspended = false
			// TODO: figure out if we need to clean up asyncify state:
			// const sstate = this.suspendState()
			// if (sstate == 1) {
			//   this.instance.exports.asyncify_stop_unwind()
			// } else if (sstate == 2) {
			//   this.instance.exports.asyncify_stop_rewind()
			// }
			if (err instanceof ProcExit) {
				this.onExit(err.status, null)
			} else if (err instanceof WebAssembly.RuntimeError) {
				this.onExit(127, null)
			} else {
				this.onExit(1, err)
			}
		}
	}

	onExit(status, err) {
		this.stdout.close()
		this.stderr.close()
		if (err) {
			dlog("Runtime exited with error", err)
			// this.doneReject(err)
			this.doneResolve([1, err])
		} else {
			dlog("Runtime exited with status", status)
			this.doneResolve([status])
		}
	}
}


enum SysOp {
	NANOSLEEP = 1,
	IPCRECV = 2,
}


syscalls[SysOp.NANOSLEEP] = (
	rt :Runtime, sec: u32, nsec: u32, rem_sec: ptr<u32>, rem_nsec: ptr<u32>) =>
{
	let ms = sec*1000.0 + nsec/1000000.0
	rt.suspend()
	// dlog(`syscall_sleep: ${microseconds/1000.0}ms`)
	// TODO: if interrupted, return remaining time:
	// - populate rem_sec_ptr & rem_nsec_ptr (pointers to u32 values)
	// - return rt.resume(-EINTR)
	setTimeout(() => rt.resume(0), ms)
}


syscalls[SysOp.IPCRECV] = (rt :Runtime, msg_ptr: ptr<void>, flags: u32) => {
	rt.suspend()
	rt.onIPCMsg = (msg) => {
		// TODO: write message to memory at msg_ptr
	}
	setTimeout(() => {
		rt.onIPCMsg(/*TODO*/)
		rt.resume(0)
	}, 500)
}
