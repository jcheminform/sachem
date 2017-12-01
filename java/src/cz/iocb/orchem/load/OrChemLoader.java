package cz.iocb.orchem.load;

import java.io.IOException;
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.util.ArrayList;
import java.util.BitSet;
import org.openscience.cdk.exception.CDKException;
import org.openscience.cdk.interfaces.IAtomContainer;
import cz.iocb.orchem.fingerprint.OrchemExtendedFingerprinter;
import cz.iocb.orchem.isomorphism.IsomorphismSort;
import cz.iocb.orchem.search.OrchemMoleculeBuilder;
import cz.iocb.orchem.shared.MoleculeCounts;
import cz.iocb.orchem.shared.MoleculeCreator;



public class OrChemLoader
{
    static class Item
    {
        int id;
        int seqid;
        String molfile;

        MoleculeCounts counts;

        Object[] similarityFingerprint;
        int similarityFingerprintCardinality;

        byte[] atoms;
        byte[] bonds;
    }


    // table names
    private static final String compoundsTable = "compounds";
    private static final String moleculesTable = "orchem_molecules";
    private static final String moleculeCountsTable = "orchem_molecule_counts";
    private static final String fingerprintTable = "orchem_fingerprint";

    // fingerprinter
    private static final ThreadLocal<OrchemExtendedFingerprinter> fingerPrinter = new ThreadLocal<OrchemExtendedFingerprinter>()
    {
        @Override
        protected OrchemExtendedFingerprinter initialValue()
        {
            return new OrchemExtendedFingerprinter();
        }
    };

    private static final int batchSize = 10000;


    public static void main(String[] args) throws Exception
    {
        try (Connection insertConnection = ConnectionPool.getConnection())
        {
            try (PreparedStatement moleculesStatement = insertConnection
                    .prepareStatement("insert into " + moleculesTable + " (seqid, id, atoms, bonds) values (?,?,?,?)"))
            {
                try (PreparedStatement moleculeCountsStatement = insertConnection.prepareStatement("insert into "
                        + moleculeCountsTable
                        + " (id, molTripleBondCount, molSCount, molOCount, molNCount, molFCount, molClCount, molBrCount, molICount, molCCount, molPCount) values (?,?,?,?,?,?,?,?,?,?,?)"))
                {
                    try (PreparedStatement similarityFingerprintStatement = insertConnection.prepareStatement(
                            "insert into " + fingerprintTable + " (id, bit_count, fp) values (?,?,?)"))
                    {
                        try (Connection selectConnection = ConnectionPool.getConnection())
                        {
                            selectConnection.setAutoCommit(false);

                            // load compounds
                            try (PreparedStatement selectStatement = selectConnection.prepareStatement(
                                    "SELECT id, molfile FROM " + compoundsTable + " ORDER BY id",
                                    ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY))
                            {
                                selectStatement.setFetchSize(batchSize);

                                try (ResultSet rs = selectStatement.executeQuery())
                                {
                                    int seqid = 0;

                                    while(true)
                                    {
                                        ArrayList<Item> items = new ArrayList<Item>(batchSize);

                                        while(items.size() < batchSize && rs.next())
                                        {
                                            Item item = new Item();

                                            item.id = rs.getInt(1);
                                            item.molfile = rs.getString(2);
                                            item.seqid = seqid++;

                                            items.add(item);
                                        }

                                        if(items.size() == 0)
                                            break;

                                        process(items);

                                        for(Item item : items)
                                        {
                                            // insert molecule binary representation
                                            moleculesStatement.setInt(1, item.seqid);
                                            moleculesStatement.setInt(2, item.id);
                                            moleculesStatement.setBytes(3, item.atoms);
                                            moleculesStatement.setBytes(4, item.bonds);
                                            moleculesStatement.addBatch();

                                            // insert molecule couts
                                            moleculeCountsStatement.setInt(1, item.id);
                                            moleculeCountsStatement.setShort(2, item.counts.molTripleBondCount);
                                            moleculeCountsStatement.setShort(3, item.counts.molSCount);
                                            moleculeCountsStatement.setShort(4, item.counts.molOCount);
                                            moleculeCountsStatement.setShort(5, item.counts.molNCount);
                                            moleculeCountsStatement.setShort(6, item.counts.molFCount);
                                            moleculeCountsStatement.setShort(7, item.counts.molClCount);
                                            moleculeCountsStatement.setShort(8, item.counts.molBrCount);
                                            moleculeCountsStatement.setShort(9, item.counts.molICount);
                                            moleculeCountsStatement.setShort(10, item.counts.molCCount);
                                            moleculeCountsStatement.setShort(11, item.counts.molPCount);
                                            moleculeCountsStatement.addBatch();

                                            // insert similarity fingerprint
                                            similarityFingerprintStatement.setInt(1, item.id);
                                            similarityFingerprintStatement.setInt(2,
                                                    item.similarityFingerprintCardinality);
                                            similarityFingerprintStatement.setArray(3, insertConnection
                                                    .createArrayOf("bigint", item.similarityFingerprint));
                                            similarityFingerprintStatement.addBatch();
                                        }

                                        moleculesStatement.executeBatch();
                                        moleculeCountsStatement.executeBatch();
                                        similarityFingerprintStatement.executeBatch();
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }


    private static void process(ArrayList<Item> items) throws CDKException, IOException
    {
        items.stream().parallel().forEach(item -> {
            try
            {
                IAtomContainer readMolecule = MoleculeCreator.getMoleculeFromMolfile(item.molfile);
                MoleculeCreator.configureMolecule(readMolecule);

                // calculate molecule binary representation
                readMolecule.setAtoms(IsomorphismSort.atomsByFrequency(readMolecule));
                OrchemMoleculeBuilder builder = new OrchemMoleculeBuilder(readMolecule);
                item.atoms = builder.atomsAsBytes();
                item.bonds = builder.bondsAsBytes();


                // calculate molecule couts
                item.counts = new MoleculeCounts(readMolecule);


                // calculate similarity fingerprint
                BitSet fp = fingerPrinter.get().getBitFingerprint(readMolecule).asBitSet();
                long[] words = fp.toLongArray();
                Object[] array = new Object[words.length];

                for(int i = 0; i < words.length; i++)
                    array[i] = new Long(words[i]);

                item.similarityFingerprint = array;
                item.similarityFingerprintCardinality = fp.cardinality();
            }
            catch (Throwable e)
            {
                e.printStackTrace();
            }
        });
    }
}
