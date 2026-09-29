// Sandbox account API — gives the starter flow something real to render
// dynamically (name + balance are templated into the USSD screen).
export default defineEventHandler((event) => {
  const q = getQuery(event)
  const msisdn = String(q.msisdn ?? '0244000000').replace(/\D/g, '')
  // deterministic per-number pseudo-account
  let h = 0
  for (const ch of msisdn) h = (h * 31 + ch.charCodeAt(0)) % 9973
  const names = ['Kwame Asante', 'Abena Owusu', 'Yao Agbeko', 'Efua Mensima', 'Nii Ankrah', 'Adjoa Boakye']
  return {
    msisdn,
    name: names[h % names.length],
    balance: (((h % 4000) / 10 + 3.5)).toFixed(2), // always renders as GHS 12.40 in ${balance} templates
    currency: 'GHS',
    bundles: ['1GB', '5GB'],
  }
})
