export default { fetch() {
  let acc = 0;
  const a = [10, 20, 30];
  for (let i = 0; i < 200000; i++) acc += a[0] + a[1] + a[2];
  return new Response(String(acc));
} };
