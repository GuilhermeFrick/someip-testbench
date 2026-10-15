// Extracao em camadas: Ethernet -> IP -> TCP/UDP -> SOME/IP -> SD.
//
// Classe concreta e SEM ESTADO: o resultado depende apenas do frame. Por isso
// e o estagio mais facil de validar, e o primeiro a fechar paridade.
#ifndef IDS_SENSOR_SOMEIP_PARSER_HPP
#define IDS_SENSOR_SOMEIP_PARSER_HPP

#include "types.hpp"

namespace ids {

class SomeipParser {
public:
    /// Decodifica o frame. Devolve false quando nao e SOME/IP ou esta
    /// malformado; nesse caso `out.valid` tambem fica false.
    ///
    /// Os payloads em `out` sao VISTAS para o buffer de `f`: nao copia nada, e
    /// nao sobrevivem ao frame.
    bool Parse(const Frame &f, ParsedPacket &out) const;
};

}  // namespace ids

#endif  // IDS_SENSOR_SOMEIP_PARSER_HPP
