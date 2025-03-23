return function(verbose)
    print("intscan_test: start")
    require("runtime/intscan_test"){ verbose = verbose }

    print("intconv_test: start")
    require("runtime/intconv_test"){ verbose = verbose }
end
