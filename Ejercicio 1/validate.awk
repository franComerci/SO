BEGIN {
    FS = ","
    getline  # salta cabecera
    max_id = 0
    dup = 0
}

{
    id = $1 + 0
    if (id <= 0) next

    if (id_seen[id]++) {
        print "❌ Error: ID duplicado", id
        dup = 1
    }

    if (id > max_id) max_id = id
}

END {
    miss = 0
    for (i = 1; i <= max_id; i++) {
        if (!(i in id_seen)) {
            print "⚠️ Falta ID", i
            miss = 1
        }
    }

    if (!dup && !miss) {
        print "✅ Validación OK: IDs correlativos y sin duplicados (orden indiferente)."
    } else if (dup) {
        print "❌ Hay duplicados detectados."
        exit 1
    } else if (miss) {
        print "⚠️ Hay IDs faltantes (posible aborto de generador)."
        exit 1
    }
}