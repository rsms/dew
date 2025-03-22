--[[
Runs the long time-blocking libc function getaddrinfo (communicates with DNS servers).
This will use dew's internal thread worker pool if needed, to avoiding blocking other tasks.
]]
__rt.main(function()
    -- syscall_addrinfo(hostname str, port=0, protocol=0, family=0, socktype=0, flags=0 uint)
    --   -> (addresses [{family=int, socktype=int, protocol=int, port=uint, addr=str}])
    --   -> (addresses nil, errmsg str)
    --
    local addrinfo, err = __rt.syscall_addrinfo("rsms.me")
    -- local addrinfo, err = __rt.syscall_addrinfo("rsms.me", 443, __rt.IPPROTO_TCP)
    if addrinfo == nil then
        print("syscall_addrinfo() error:", err)
    else
        print("syscall_addrinfo() resulted in " .. #addrinfo .. " addresses:")
        for i, addr in ipairs(addrinfo) do
            local family
            if     addr.family == __rt.AF_LOCAL then family = "LOCAL"
            elseif addr.family == __rt.AF_INET  then family = "INET"
            elseif addr.family == __rt.AF_INET6 then family = "INET6"
            elseif addr.family == __rt.AF_ROUTE then family = "ROUTE"
            elseif addr.family == __rt.AF_VSOCK then family = "VSOCK"
            else family = tostring(addr.family) end

            local protocol
            if     addr.protocol == __rt.IPPROTO_IP      then protocol = "IP"
            elseif addr.protocol == __rt.IPPROTO_ICMP    then protocol = "ICMP"
            elseif addr.protocol == __rt.IPPROTO_TCP     then protocol = "TCP"
            elseif addr.protocol == __rt.IPPROTO_UDP     then protocol = "UDP"
            elseif addr.protocol == __rt.IPPROTO_IPV6    then protocol = "IPV6"
            elseif addr.protocol == __rt.IPPROTO_ROUTING then protocol = "ROUTING"
            elseif addr.protocol == __rt.IPPROTO_RAW     then protocol = "RAW"
            else protocol = tostring(addr.protocol) end

            local socktype
            if     addr.socktype == __rt.SOCK_STREAM then socktype = "SOCK_STREAM"
            elseif addr.socktype == __rt.SOCK_DGRAM  then socktype = "SOCK_DGRAM"
            elseif addr.socktype == __rt.SOCK_RAW    then socktype = "SOCK_RAW"
            else socktype = tostring(addr.socktype) end

            print(string.format(
                "- [%d] %s %d (family=%s protocol=%s socktype=%s)",
                i, addr.addr, addr.port, family, protocol, socktype))
        end
    end
end)
