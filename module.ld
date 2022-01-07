SECTIONS
{
	.text : {
		*(.text*);
		*(.gnu.linkonce.t.*);
	}
	
	.ctors : {
		*(.ctors);
	}
	
	.dtors : {
		*(.dtors);
	}
	
	.rodata : {
		*(.rodata*);
		*(.gnu.linkonce.r.*);
	}
	
	.data : {
		*(.data*);
		*(.gnu.linkonce.d.*);
	}
	
	.bss (NOLOAD) : {
		*(COMMON);
		*(.scommon*);
		*(.bss*);
		*(.gnu.linkonce.b*);
	}
	
	/* Discard everything not specifically mentioned above. */
	/DISCARD/ :
	{
		*(*);
	}
}