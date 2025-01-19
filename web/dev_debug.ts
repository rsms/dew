export function dlog(...args) {
	console.debug(...args)
}

export function assert(cond) {
	if (!cond)
		throw new Error(`Assertion failed`)
}
