let total = 0; const words = []
for (let i = 0; i < 1_000_000; i++) {
  const s = `item-${i}: ${i * 0.5}`
  total += s.length
  if (i % 100_000 === 0) words.push(s)
}
const joined = words.join(",")
console.log(total, joined.split(",").length, joined.toUpperCase().indexOf("ITEM-500000"))
