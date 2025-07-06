(function (t)

    -- semicolon at the end; all three forms equivalent
    t("1 + 2",   {{TOK_INT,1}, TOK_PLUS, {TOK_INT,2}, TOK_SEMI})
    t("1 + 2\n", {{TOK_INT,1}, TOK_PLUS, {TOK_INT,2}, TOK_SEMI})
    t("1 + 2;",  {{TOK_INT,1}, TOK_PLUS, {TOK_INT,2}, TOK_SEMI})

    -- TODO: more tests

end)(function(input, expected_tokens)
    unit = unit_create_buf("<stdin>", input)
    tokens = tokenize_unit(unit)

    -- check expectation
    do
        local i = 1
        local unexpected_count = 0
        for tok, srcpos, value, meta in token_iterator(tokens) do
            local want_tok, want_value = expected_tokens[i], nil
            if want_tok == nil then
                unexpected_count = unexpected_count + 1
                printf("token #%d: unexpected extra token '%s'", i, token_names[tok])
                break
            end
            if type(want_tok) ~= "number" then -- tok, val
                want_value = tostring(want_tok[2])
                want_tok = want_tok[1]
            end
            -- print(token_names[want_tok], token_names[tok], want_value, value)
            if want_tok ~= tok then
                unexpected_count = unexpected_count + 1
                if unexpected_count == 1 then
                    printf("token #%d: unexpected type '%s' (want '%s')",
                           i, token_names[tok], token_names[want_tok])
                end
            elseif want_value ~= nil then
                value = tostring(value)
                if want_value ~= value then
                    unexpected_count = unexpected_count + 1
                    if unexpected_count == 1 then
                        printf("token #%d %s: unexpected value: \"%s\" (want \"%s\")",
                               i, token_names[tok], value, want_value)
                    end
                end
            end
            i = i + 1
        end
        if unexpected_count == 0 and #tokens < #expected_tokens then
            local want_tok = expected_tokens[i]
            if type(want_tok) ~= "number" then
                want_tok = want_tok[1]
            end
            printf("missing token #%d '%s'", i, token_names[want_tok])
        elseif unexpected_count == 0 then
            return
        end
    end

    -- failure
    local buf = __rt.buf_create(512)
    local function write(s) buf:append(s) end
    write("  input:\n    " .. input .. "\n")
    write("  expected output:")
    for i, tok in ipairs(expected_tokens) do
        local value = ""
        if type(tok) ~= "number" then -- tok, val
            value = tostring(tok[2])
            tok = tok[1]
        end
        write(fmt("\n    #%-3d %-8s %s", i, token_names[tok], value))
    end
    write("\n  actual output:")
    local i = 1
    for tok, srcpos, value, meta in token_iterator(tokens) do
        write(fmt("\n    #%-3d %-8s %s", i, token_names[tok], tostring(value)))
        i = i + 1
    end
    FAIL(buf:str(), 2)
end)
