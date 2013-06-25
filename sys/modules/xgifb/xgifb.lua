require 'hw'
require 'core'

function xgifbMatch(pa, devs, num)
	 if hw.pci_matchbyid(pa, devs, num) == 1 then
	    return 100
	 else
	    return 0
	 end
end
function xgifbAttach(sc, pa)
	 local res
	 local fbsize, mmiosize, iosize
	 local sc_iot, sc_ioh, mmio_iot, mmio_ioh, iot, ioh

	 hw.pci_aprint_devinfo(pa)

	 res, fbsize = hw.pci_mapreg_map(pa, 0, hw.PCI_MAPREG_TYPE_MEM,
	    hw.BUS_SPACE_MAP_LINEAR, sc.sc_iot, sc.sc_ioh)
	 if res ~= 0 then
	    core.print(sc.dv_xname .. ": can't map frame buffer\n")
	    return
	 end
	 res, mmiosize = hw.pci_mapreg_map(pa, 4, hw.PCI_MAPREG_TYPE_MEM,
	   0, sc.mmio_iot, sc.mmio_ioh)
	 if res ~= 0 then
	    core.print(sc.dv_xname .. ": can't map mmio area\n")
	    return
	 end
	 res, iosize = hw.pci_mapreg_map(pa, 8, hw.PCI_MAPREG_TYPE_IO,
	    0, sc.iot, sc.ioh)
	 if res ~= 0 then
	    core.print(sc.dv_xname .. ": can't map registers\n")
	    return
	 end

	 -- core.print(sc.dv_xname .. ': ' .. fbsize .. ' ' .. mmiosize .. ' ' .. iosize .. '\n')
end

function onClose()
end
