package org.t2dos.term

import org.junit.Assert.assertEquals
import org.junit.Test

class BoardStoreTest {
    @Test fun roundTrip() {
        val list = listOf(
            Board(1, "Workbench \"main\"", "192.168.1.150", 23, 14f, "c2VhbGVk+/="),
            Board(2, "två\ttabs\nand \\ lines", "board.local", 2323, 11.5f, null),
        )
        assertEquals(list, BoardStore.decode(BoardStore.encode(list)))
    }

    @Test fun emptyIsEmpty() = assertEquals(emptyList<Board>(), BoardStore.decode(""))
}
