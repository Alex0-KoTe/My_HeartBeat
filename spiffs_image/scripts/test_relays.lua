-- Тест реле MCP23017 (без циклов, линейно)
print("=== ТЕСТ РЕЛЕ (линейный режим) ===")

-- Реле 0
print("Включаем RL0...")
RL0(1)
delay(500)
print("Выключаем RL0")
RL0(0)
delay(200)

-- Реле 1
print("Включаем RL1...")
RL1(1)
delay(500)
print("Выключаем RL1")
RL1(0)
delay(200)

-- Реле 2
print("Включаем RL2...")
RL2(1)
delay(500)
print("Выключаем RL2")
RL2(0)
delay(200)

-- Реле 3
print("Включаем RL3...")
RL3(1)
delay(500)
print("Выключаем RL3")
RL3(0)
delay(200)

-- Реле 4
print("Включаем RL4...")
RL4(1)
delay(500)
print("Выключаем RL4")
RL4(0)
delay(200)

-- Реле 5
print("Включаем RL5...")
RL5(1)
delay(500)
print("Выключаем RL5")
RL5(0)
delay(200)

-- Реле 6
print("Включаем RL6...")
RL6(1)
delay(500)
print("Выключаем RL6")
RL6(0)
delay(200)

-- Реле 7
print("Включаем RL7...")
RL7(1)
delay(500)
print("Выключаем RL7")
RL7(0)

print("=== ТЕСТ ЗАВЕРШЕН ===")