export function dlog(...args) {
	console.debug(...args)
}

export function assert(cond, ...args) {
	if (!cond) {
		let msg = `Assertion failed`
		if (args) msg += ': ' + Array.from(args).join(" ")
		throw new Error(msg)
	}
}
