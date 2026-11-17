package org.t2dos.term

import org.junit.Assert.assertEquals
import org.junit.Test

class LayoutTest {
    @Test fun fits() = assertEquals(Pair(40, 27), Layout.grid(1080, 1500, 27f, 54f))
    @Test fun tinyHasAFloor() = assertEquals(Pair(20, 4), Layout.grid(10, 10, 27f, 54f))
    @Test fun hugeHasACeiling() = assertEquals(Pair(1024, 1024), Layout.grid(100000, 100000, 1f, 1f))
}
