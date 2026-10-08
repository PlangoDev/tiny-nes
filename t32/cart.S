    .section .rodata
    .globl cart, cart_end
    .balign 4
cart:
    .incbin CART
cart_end:
